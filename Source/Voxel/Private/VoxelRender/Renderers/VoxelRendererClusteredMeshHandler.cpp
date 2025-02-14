// Copyright 2021 Phyronnaz

#include "VoxelRendererClusteredMeshHandler.h"
#include "VoxelAsyncWork.h"
#include "VoxelPool.h"
#include "VoxelDebug/VoxelDebugManager.h"
#include "VoxelRender/IVoxelRenderer.h"
#include "VoxelRender/VoxelChunkMaterials.h"
#include "VoxelRender/VoxelChunkMesh.h"
#include "VoxelRender/VoxelProceduralMeshComponent.h"
#include "VoxelRender/VoxelProcMeshBuffers.h"
#include "VoxelUtilities/VoxelThreadingUtilities.h"

#include "Async/Async.h"

class FVoxelClusteredMeshMergeWork : public FVoxelAsyncWork
{
public:
	static FVoxelClusteredMeshMergeWork* Create(
		FVoxelRendererClusteredMeshHandler& Handler,
		FVoxelRendererClusteredMeshHandler::FClusterRef ClusterRef)
	{
		auto* Cluster = Handler.GetCluster(ClusterRef);
		check(Cluster);
		return new FVoxelClusteredMeshMergeWork(
			ClusterRef,
			Cluster->Position,
			Handler,
			Cluster->UpdateIndex.ToSharedRef(),
			Cluster->ChunkMeshesToBuild);
	}

private:
	const FVoxelRendererClusteredMeshHandler::FClusterRef ClusterRef;
	const FIntVector Position;
	const FVoxelRuntimeSettings Settings;
	const TVoxelWeakPtr<FVoxelRendererClusteredMeshHandler> Handler;

	const TMap<uint64, TVoxelSharedPtr<const FVoxelChunkMeshesToBuild>> MeshesToBuild;
	const TVoxelSharedRef<FThreadSafeCounter> UpdateIndexPtr;
	const int32 UpdateIndex;

	FVoxelClusteredMeshMergeWork(
		FVoxelRendererClusteredMeshHandler::FClusterRef ClusterRef,
		const FIntVector& Position,
		FVoxelRendererClusteredMeshHandler& Handler,
		const TVoxelSharedRef<FThreadSafeCounter>& UpdateIndexPtr,
		const TMap<uint64, TVoxelSharedPtr<const FVoxelChunkMeshesToBuild>>& MeshesToBuild)
		: FVoxelAsyncWork(STATIC_FNAME("FVoxelClusteredMeshMergeWork"), EVoxelTaskType::MeshMerge, EPriority::Null,
		                  true)
		  , ClusterRef(ClusterRef)
		  , Position(Position)
		  , Settings(Handler.Renderer.Settings)
		  , Handler(StaticCastSharedRef<FVoxelRendererClusteredMeshHandler>(Handler.AsShared()))
		  , MeshesToBuild(MeshesToBuild)
		  , UpdateIndexPtr(UpdateIndexPtr)
		  , UpdateIndex(UpdateIndexPtr->GetValue())
	{
	}

	virtual ~FVoxelClusteredMeshMergeWork() override = default;

	virtual void DoWork() override
	{
		if (UpdateIndexPtr->GetValue() > UpdateIndex)
		{
			// Canceled
			return;
		}
		FVoxelChunkMeshesToBuild RealMap;
		for (auto& ChunkIt : MeshesToBuild)
		{
			// Key is ChunkId = useless here
			for (auto& MeshIt : *ChunkIt.Value)
			{
				auto& MeshMap = RealMap.FindOrAdd(MeshIt.Key);
				for (auto& SectionIt : MeshIt.Value)
				{
					MeshMap.FindOrAdd(SectionIt.Key).Append(SectionIt.Value);
				}
			}
		}
		auto BuiltMeshes = FVoxelRenderUtilities::BuildMeshes_AnyThread(RealMap, Settings, Position, *UpdateIndexPtr,
		                                                                UpdateIndex);
		if (!BuiltMeshes.IsValid())
		{
			// Canceled
			return;
		}
		auto HandlerPinned = Handler.Pin();
		if (HandlerPinned.IsValid())
		{
			// Queue callback
			HandlerPinned->MeshMergeCallback(ClusterRef, UpdateIndex, MoveTemp(BuiltMeshes));
		}
	}
};

FVoxelRendererClusteredMeshHandler::~FVoxelRendererClusteredMeshHandler()
{
	FlushBuiltDataQueue();
	FlushActionQueue(MAX_dbl);

	ensure(ChunkInfos.Num() == 0);
	ensure(Clusters.Num() == 0);
}

IVoxelRendererMeshHandler::FChunkId FVoxelRendererClusteredMeshHandler::AddChunkImpl(
	int32 LOD, const FIntVector& Position)
{
	return ChunkInfos.Add(FChunkInfo::Create(LOD, Position));
}

void FVoxelRendererClusteredMeshHandler::ApplyAction(const FAction& Action)
{
	VOXEL_FUNCTION_COUNTER();

	const FChunkId ChunkId{Action.ChunkId};

	switch (Action.Action)
	{
	case EAction::UpdateChunk:
		{
			check(Action.UpdateChunk().InitialCall.MainChunk);
			const auto& MainChunk = *Action.UpdateChunk().InitialCall.MainChunk;
			const auto* TransitionChunk = Action.UpdateChunk().InitialCall.TransitionChunk;

			// This should never happen, as the chunk should be removed instead
			ensure(!MainChunk.IsEmpty() || (TransitionChunk && !TransitionChunk->IsEmpty()));

			auto& ChunkInfo = ChunkInfos[ChunkId];

			if (!ChunkInfo.ClusterId.IsValid())
			{
				const FClusterRef ClusterRef = FindOrCreateCluster(ChunkInfo.LOD, ChunkInfo.Position);
				ChunkInfo.ClusterId = ClusterRef.ClusterId;
				Clusters[ChunkInfo.ClusterId].NumChunks++;
			}

			auto& Cluster = Clusters[ChunkInfo.ClusterId];
			if (!Cluster.Materials.IsValid())
			{
				Cluster.Materials = MakeShared<FVoxelChunkMaterials>();
			}
			if (!Cluster.UpdateIndex.IsValid())
			{
				Cluster.UpdateIndex = MakeVoxelShared<FThreadSafeCounter>();
			}

			// Cancel any previous build task
			// Note: we do not clear the built data, as it could still be used
			// The added cost of applying the update is probably worth it compared to stalling the entire queue waiting for an update
			Cluster.UpdateIndex->Increment();

			// Find the meshes to build (= copying & merging mesh buffers to proc mesh buffers)
			FVoxelChunkMeshesToBuild MeshesToBuild = FVoxelRenderUtilities::GetMeshesToBuild(
				ChunkInfo.LOD,
				ChunkInfo.Position,
				Renderer,
				Action.UpdateChunk().InitialCall.ChunkSettings,
				*Cluster.Materials,
				MainChunk,
				TransitionChunk,
				{});

			Cluster.ChunkMeshesToBuild.FindOrAdd(ChunkInfo.UniqueId) = MakeVoxelShared<FVoxelChunkMeshesToBuild>(
				MoveTemp(MeshesToBuild));

			// Start a task to asynchronously build them
			auto* Task = FVoxelClusteredMeshMergeWork::Create(*this, {ChunkInfo.ClusterId, Cluster.UniqueId});
			Renderer.GetSubsystemChecked<FVoxelPool>().QueueTask(Task);

			FAction NewAction;
			NewAction.Action = EAction::UpdateChunk;
			NewAction.ChunkId = Action.ChunkId;
			NewAction.UpdateChunk().AfterCall.UpdateIndex = Cluster.UpdateIndex->GetValue();
			NewAction.UpdateChunk().AfterCall.DistanceFieldVolumeData = Action.UpdateChunk().InitialCall.MainChunk->
			                                                                   GetDistanceFieldVolumeData();
			ActionQueue.Enqueue(NewAction);
			break;
		}
	case EAction::RemoveChunk:
		{
			auto& ChunkInfo = ChunkInfos[Action.ChunkId];

			if (!ChunkInfo.ClusterId.IsValid())
			{
				// No cluster = no mesh, just remove it
				ChunkInfos.RemoveAt(Action.ChunkId);
				return;
			}

			// For remove, we trigger an update
			// and then queue a remove action

			auto& Cluster = Clusters[ChunkInfo.ClusterId];

			// Cancel any previous build task
			// Note: we do not clear the built data, as it could still be used
			// The added cost of applying the update is probably worth it compared to stalling the entire queue waiting for an update
			Cluster.UpdateIndex->Increment();

			ensure(Cluster.ChunkMeshesToBuild.Remove(ChunkInfo.UniqueId) == 1); // We must have some mesh

			// Start a task to asynchronously build them
			auto* Task = FVoxelClusteredMeshMergeWork::Create(*this, {ChunkInfo.ClusterId, Cluster.UniqueId});
			Renderer.GetSubsystemChecked<FVoxelPool>().QueueTask(Task);

			FAction NewAction;
			NewAction.Action = EAction::UpdateChunk;
			NewAction.ChunkId = Action.ChunkId;
			NewAction.UpdateChunk().AfterCall.UpdateIndex = Cluster.UpdateIndex->GetValue();
			NewAction.UpdateChunk().AfterCall.DistanceFieldVolumeData = nullptr;
			ActionQueue.Enqueue(NewAction);
			// Enqueue remove as well
			ActionQueue.Enqueue(Action);
		}
	case EAction::SetTransitionsMaskForSurfaceNets:
		{
			break;
		}
	case EAction::DitherChunk:
	case EAction::ResetDithering:
	// These 2 should be done by an UpdateChunk
	case EAction::HideChunk:
	case EAction::ShowChunk:
	default: ensure(false);
	}
}

void FVoxelRendererClusteredMeshHandler::ClearChunkMaterials()
{
	for (auto& Cluster : Clusters)
	{
		if (Cluster.Materials.IsValid())
		{
			Cluster.Materials->Reset();
		}
	}
}

void FVoxelRendererClusteredMeshHandler::Tick(double MaxTime)
{
	VOXEL_FUNCTION_COUNTER();

	IVoxelRendererMeshHandler::Tick(MaxTime);

	FlushBuiltDataQueue();
	FlushActionQueue(MaxTime);

	Renderer.GetSubsystemChecked<FVoxelDebugManager>().ReportMeshActionQueueNum(ActionQueue.Num());
}

FVoxelRendererClusteredMeshHandler::FClusterRef FVoxelRendererClusteredMeshHandler::FindOrCreateCluster(
	int32 LOD,
	const FIntVector& Position)
{
	const int32 ClusterSize = FMath::Clamp<uint64>(
		static_cast<uint64>(Renderer.Settings.RenderOctreeChunkSize * Renderer.Settings.MergedChunksClusterSize) << LOD,
		1, MAX_int32);
	// Try to not split chunks on the origin
	const FIntVector QueryPosition = Position + ClusterSize / 2;
	const FIntVector Key = FVoxelUtilities::DivideFloor(QueryPosition, ClusterSize);

	auto& ClusterRef = ClustersMap[LOD].FindOrAdd(Key);
	if (!GetCluster(ClusterRef))
	{
		ClusterRef.ClusterId = Clusters.Add(FCluster::Create(LOD, Key * ClusterSize));
		ClusterRef.UniqueId = Clusters[ClusterRef.ClusterId].UniqueId;
	}
	check(GetCluster(ClusterRef));
	return ClusterRef;
}

void FVoxelRendererClusteredMeshHandler::FlushBuiltDataQueue()
{
	VOXEL_FUNCTION_COUNTER();

	// Copy built data from async task callbacks to the chunk infos
	// Should be fast enough to not require checking the time
	FBuildCallback Callback;
	while (CallbackQueue.Dequeue(Callback))
	{
		auto& BuiltData = Callback.BuiltData;

		if (!ensure(BuiltData.BuiltMeshes.IsValid()))
		{
			continue;
		}

		auto* Cluster = GetCluster(Callback.ClusterRef);
		if (Cluster && BuiltData.UpdateIndex >= Cluster->UpdateIndex->GetValue())
		{
			// Not outdated
			ensure(BuiltData.UpdateIndex == Cluster->UpdateIndex->GetValue());
			Cluster->BuiltData = MoveTemp(BuiltData);
		}
	}
}

void FVoxelRendererClusteredMeshHandler::FlushActionQueue(double MaxTime)
{
	VOXEL_FUNCTION_COUNTER();

	FAction Action;
	// Peek: if UpdateChunk isn't ready yet we don't want to pop the action
	while (ActionQueue.Peek(Action) && FPlatformTime::Seconds() < MaxTime)
	{
		auto& ChunkInfo = ChunkInfos[Action.ChunkId];

		if (IsDestroying() && Action.Action != EAction::RemoveChunk)
		{
			ActionQueue.Pop();
			continue;
		}

		switch (Action.Action)
		{
		case EAction::UpdateChunk:
			{
				if (!ensure(ChunkInfo.ClusterId.IsValid()))
				{
					continue; // Not possible if UpdateChunk was called
				}
				auto& Cluster = Clusters[ChunkInfo.ClusterId];
				const int32 WantedUpdateIndex = Action.UpdateChunk().AfterCall.UpdateIndex;
				if (Cluster.MeshUpdateIndex >= WantedUpdateIndex)
				{
					// Already updated
					// This happens when a previous UpdateChunk used the built data we triggered
					break;
				}
				if (WantedUpdateIndex > Cluster.BuiltData.UpdateIndex)
				{
					// Not built yet, wait

					if (Cluster.BuiltData.UpdateIndex != -1)
					{
						// Stored built data is outdated, clear it to save memory
						ensure(Cluster.BuiltData.BuiltMeshes.IsValid());
						Cluster.BuiltData.BuiltMeshes.Reset();
						Cluster.BuiltData.UpdateIndex = -1;
					}

					return;
				}

				// Move to clear the built data value
				const auto BuiltMeshes = MoveTemp(Cluster.BuiltData.BuiltMeshes);
				Cluster.MeshUpdateIndex = Cluster.BuiltData.UpdateIndex;
				Cluster.BuiltData.UpdateIndex = -1;

				if (!ensure(BuiltMeshes.IsValid()))
				{
					continue;
				}

				int32 MeshIndex = 0;
				CleanUp(Cluster.Meshes);
				// Apply built meshes
				for (auto& BuiltMesh : *BuiltMeshes)
				{
					const FVoxelMeshConfig& MeshConfig = BuiltMesh.Key;
					if (Cluster.Meshes.Num() <= MeshIndex)
					{
						// Not enough meshes to render the built mesh, allocate new ones
						auto* NewMesh = GetNewMesh(Action.ChunkId, Cluster.Position, Cluster.LOD);
						if (!ensureVoxelSlow(NewMesh))
						{
							return;
						}
						Cluster.Meshes.Add(NewMesh);
					}

					auto& Mesh = *Cluster.Meshes[MeshIndex];
					MeshConfig.ApplyTo(Mesh);

					Mesh.SetDistanceFieldData(nullptr);
					Mesh.SetReceivesDecals(MeshConfig.bReceivesDecals);
					Mesh.ClearSections(EVoxelProcMeshSectionUpdate::DelayUpdate);
					for (auto& Section : BuiltMesh.Value)
					{
						Mesh.AddProcMeshSection(Section.Key, MoveTemp(Section.Value),
						                        EVoxelProcMeshSectionUpdate::DelayUpdate);
					}
					Mesh.FinishSectionsUpdates();

					MeshIndex++;
				}

				// Clear unused meshes
				for (; MeshIndex < Cluster.Meshes.Num(); MeshIndex++)
				{
					auto& Mesh = *Cluster.Meshes[MeshIndex];
					Mesh.SetDistanceFieldData(nullptr);
					Mesh.ClearSections(EVoxelProcMeshSectionUpdate::UpdateNow);
				}

				// Handle distance fields
				const auto& DistanceFieldVolumeData = Action.UpdateChunk().AfterCall.DistanceFieldVolumeData;
				if (DistanceFieldVolumeData.IsValid())
				{
#if 0
				// Use the first mesh to hold the distance field data
				// We should always have at least one mesh, else the chunk should have been removed instead of updated
				if (ensure(Cluster.Meshes.Num() > 0))
				{
					Cluster.Meshes[0]->SetDistanceFieldData(DistanceFieldVolumeData);
				}
#endif
					// TODO: No distance fields with merging = on
				}
				break;
			}
		case EAction::RemoveChunk:
			{
				if (ChunkInfo.ClusterId.IsValid())
				{
					auto& Cluster = Clusters[ChunkInfo.ClusterId];
					Cluster.NumChunks--;
					ensure(Cluster.NumChunks >= 0);
					if (Cluster.NumChunks == 0)
					{
						for (auto& Mesh : CleanUp(Cluster.Meshes))
						{
							RemoveMesh(*Mesh);
						}
						Clusters.RemoveAt(ChunkInfo.ClusterId);
					}
				}
				ChunkInfos.RemoveAt(Action.ChunkId);
				break;
			}
		case EAction::ShowChunk:
		case EAction::HideChunk:
		case EAction::SetTransitionsMaskForSurfaceNets:
		case EAction::DitherChunk:
		case EAction::ResetDithering:
		default: ensure(false);
		}

		if (CVarLogActionQueue.GetValueOnGameThread() != 0)
		{
			LOG_VOXEL(Log, TEXT("ActionQueue: LOD: %d; %s; Position: %s"), ChunkInfo.LOD, *Action.ToString(),
			          *ChunkInfo.Position.ToString());
		}

		ActionQueue.Pop();
	}
}

void FVoxelRendererClusteredMeshHandler::MeshMergeCallback(FClusterRef ClusterRef, int32 UpdateIndex,
                                                           TUniquePtr<FVoxelBuiltChunkMeshes> BuiltMeshes)
{
	CallbackQueue.Enqueue({ClusterRef, FClusterBuiltData{UpdateIndex, MoveTemp(BuiltMeshes)}});
}
