// For copyright see LICENSE in EnvironmentProject root dir, or:
//https://github.com/UE4-OceanProject/OceanProject/blob/Master-Environment-Project/LICENSE

#include "BuoyantMesh/BuoyantMeshComponent.h"
#include "OceanPlugin/Public/OceanManager.h"
#include "PhysicsEngine/BodySetup.h"
#include "BuoyantMesh/BuoyantMeshTriangle.h"
#include "BuoyantMesh/BuoyantMeshSubtriangle.h"
#include "BuoyantMesh/WaterHeightmapComponent.h"
#include "Engine/World.h"
#include "DrawDebugHelpers.h"
#include "EngineUtils.h"
#include "ProceduralMeshComponent/Public/KismetProceduralMeshLibrary.h"


using FForce = UBuoyantMeshComponent::FForce;

// Sets default values for this component's properties
UBuoyantMeshComponent::UBuoyantMeshComponent()
{
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
	PrimaryComponentTick.bCanEverTick = true;
	bWantsInitializeComponent = true;
	bAutoActivate = true;
	UActorComponent::SetComponentTickEnabled(true);
}

float UBuoyantMeshComponent::GetHeightAboveWater(const FVector& Position) const
{
	float WaterHeight = 0.f;
	if (IsValid(OceanManager))
	{
		if (bUseWaterPatch && IsValid(WaterHeightmap))
		{
			WaterHeight = WaterHeightmap->GetHeightAtPosition(Position);
		}
		else
		{
			WaterHeight = OceanManager->GetWaveHeight(Position, World);
		}
	}
	return Position.Z - WaterHeight;
}

UPrimitiveComponent* UBuoyantMeshComponent::GetParentPrimitive() const
{
	if (IsValid(GetAttachParent()))
	{
		const auto PrimitiveComponent = Cast<UPrimitiveComponent>(GetAttachParent());
		if (PrimitiveComponent)
		{
			return PrimitiveComponent;
		}
	}
	return nullptr;
}

AOceanManager* UBuoyantMeshComponent::FindOceanManager() const
{
	for (auto Actor : TActorRange<AOceanManager>(GetWorld()))
	{
		return Actor;
	}
	return nullptr;
}

UWaterHeightmapComponent* UBuoyantMeshComponent::FindWaterHeightmap() const
{
	TInlineComponentArray<UWaterHeightmapComponent*> WaterHeightmaps;
	const auto Owner = GetOwner();
	check(Owner);
	Owner->GetComponents(WaterHeightmaps);
	if (WaterHeightmaps.Num() > 0)
	{
		return WaterHeightmaps[0];
	}
	else
	{
		return nullptr;
	}
}

void UBuoyantMeshComponent::SetupTickOrder()
{
	// This component needs to tick before the updated component.
	if (UpdatedComponent != this)
	{
		UpdatedComponent->PrimaryComponentTick.AddPrerequisite(this, PrimaryComponentTick);
	}

	// The water heightmap needs to tick before this component.
	if (WaterHeightmap)
	{
		PrimaryComponentTick.AddPrerequisite(WaterHeightmap, WaterHeightmap->PrimaryComponentTick);
	}
}

void UBuoyantMeshComponent::Initialize()
{
	if (UpdatedComponent == nullptr)
	{
		const auto ParentPrimitive = GetParentPrimitive();
		UpdatedComponent = ParentPrimitive ? ParentPrimitive : this;
	}

	if (!OceanManager)
	{
		OceanManager = FindOceanManager();
	}

	WaterHeightmap = FindWaterHeightmap();

	SetupTickOrder();

	TriangleMeshes = TMeshUtilities::GetTriangleMeshes(this);

	World = GetWorld();
	GravityMagnitude = FMath::Abs(World->GetGravityZ());

	SetMassProperties();
}


void UBuoyantMeshComponent::SetMassProperties()
{
	if (UpdatedComponent)
	{
		if (bOverrideMeshDensity)
		{
			const auto MeshVolume = TMathUtilities::MeshVolume(this);
			const auto ComputedMass = MeshDensity * MeshVolume;
			UpdatedComponent->SetMassOverrideInKg(NAME_None, ComputedMass);
		}

		if (bOverrideMass)
		{
			UpdatedComponent->SetMassOverrideInKg(NAME_None, Mass);
		}
	}
}

void UBuoyantMeshComponent::DrawDebugTriangle(
    UWorld* World, const FVector& A, const FVector& B, const FVector& C, const FColor& Color, const float Thickness)
{
	DrawDebugLine(World, A, B, Color, false, -1.f, 0, Thickness);
	DrawDebugLine(World, B, C, Color, false, -1.f, 0, Thickness);
	DrawDebugLine(World, C, A, Color, false, -1.f, 0, Thickness);
}

// Called every frame
void UBuoyantMeshComponent::TickComponent(float DeltaTime,
                                          ELevelTick TickType,
                                          FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bHasInitialized)
	{
		Initialize();
		bHasInitialized = true;

		if (!IsValid(UpdatedComponent) || !UpdatedComponent->IsSimulatingPhysics())
		{
			UE_LOG(LogTemp,
			       Error,
			       TEXT("BuoyantMeshComponent has no updated component set up. Use a ")
			           TEXT("parent component with \"Simulate Physics\" turned on."));
			return;
		}
	}

	ApplyMeshForces();
}


void UBuoyantMeshComponent::ApplyMeshForces()
{
	auto debugWorld = GetWorld();

	const auto LocalToWorld = GetComponentTransform();

	for (const auto &TriangleMesh : TriangleMeshes)
	{
		TArray<FBuoyantMeshVertex> BuoyantMeshVertices{};
		for (const auto &Vertex : TriangleMesh.Vertices)
		{
			const auto WorldVertex = LocalToWorld.TransformPosition(Vertex);
			BuoyantMeshVertices.Emplace(WorldVertex, GetHeightAboveWater(WorldVertex));
		}

		const auto TriangleCount = TriangleMesh.TriangleVertexIndices.Num() / 3;
		for (int32 i = 0; i < TriangleCount; ++i)
		{
			const auto A = BuoyantMeshVertices[TriangleMesh.TriangleVertexIndices[i * 3 + 0]];
			const auto B = BuoyantMeshVertices[TriangleMesh.TriangleVertexIndices[i * 3 + 2]];
			const auto C = BuoyantMeshVertices[TriangleMesh.TriangleVertexIndices[i * 3 + 1]];

			if (bDrawTriangles)
			{
				DrawDebugTriangle(debugWorld, A.Position, B.Position, C.Position, FColor::White, 4.f);
			}

			const auto Triangle = FBuoyantMeshTriangle::FromClockwiseVertices(A, B, C);

			const auto SubTriangles = Triangle.GetSubmergedPortion(debugWorld, bDrawWaterline);

			for (const auto& SubTriangle : SubTriangles)
			{
				if (bDrawSubtriangles)
				{
					DrawDebugTriangle(debugWorld, SubTriangle.A, SubTriangle.B, SubTriangle.C, FColor::Yellow, 6.f);
				}

				const auto SubtriangleForce = GetSubmergedTriangleForce(SubTriangle, Triangle.Normal);
				ApplyMeshForce(SubtriangleForce);
			}
		}
	}
}

void UBuoyantMeshComponent::ApplyMeshForce(const FForce& Force)
{
	const auto ForceVector = bVerticalForcesOnly ? FVector{0.f, 0.f, Force.Vector.Z} : Force.Vector;
	const auto bIsValidForce = !ForceVector.IsNearlyZero() && !ForceVector.ContainsNaN();
	if (!bIsValidForce) return;

	UpdatedComponent->AddForceAtLocation(ForceVector, Force.Point);
	if (bDrawForceArrows)
	{
		DrawDebugLine(World, Force.Point - (ForceVector * ForceArrowSize * 0.0001f), Force.Point, FColor::Blue);
	}
}

FForce UBuoyantMeshComponent::GetSubmergedTriangleForce(const FBuoyantMeshSubtriangle& Subtriangle,
                                                        const FVector& TriangleNormal) const
{
	const auto CenterPosition = Subtriangle.GetCenter();
	const FBuoyantMeshVertex CenterVertex{CenterPosition, GetHeightAboveWater(CenterPosition)};
	const auto TriangleArea = Subtriangle.GetArea();
	if (FMath::IsNearlyZero(TriangleArea)) return FForce{FVector::ZeroVector, FVector::ZeroVector};

	FVector Force = FVector::ZeroVector;

	if (bUseStaticForces)
	{
		const auto StaticForce = FBuoyantMeshSubtriangle::GetHydrostaticForce(
		    WaterDensity, GravityMagnitude, CenterVertex, TriangleNormal, TriangleArea);
		Force += StaticForce;
	}

	if (bUseDynamicForces)
	{
		const auto CenterVelocity = UpdatedComponent->GetBodyInstance()->GetUnrealWorldVelocityAtPoint(CenterPosition);
		const auto DynamicForce = FBuoyantMeshSubtriangle::GetHydrodynamicForce(
		    WaterDensity, CenterPosition, CenterVelocity, TriangleNormal, TriangleArea);
		Force += DynamicForce;
	}

	return FForce{Force, CenterPosition};
}

// Reference: https://wiki.unrealengine.com/Accessing_mesh_triangles_and_vertex_positions_in_build --> Not working in UE5 since Physx is removed
// We convert the mesh to Procedural Mesh to get vertices/triangles data but Allow CPU Access must be enabled in static mesh properties to work in cooked builds
TArray<FTriangleMesh> TMeshUtilities::GetTriangleMeshes(UStaticMeshComponent* StaticMeshComponent)
{
	if (!StaticMeshComponent) return {};

	#if !WITH_EDITOR
	if (!StaticMeshComponent->GetStaticMesh()->bAllowCPUAccess) return {};
	#endif

	TArray<FTriangleMesh> Meshes;
	int32 Sections = StaticMeshComponent->GetStaticMesh()->GetNumSections(0);
	TArray<FVector> CurrentSectionVertices;
	TArray<FVector> TotalSectionsVertices;
	TArray<int32> CurrentSectionTriangles;
	TArray<int32> TotalSectionsTriangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UV;
	TArray<FProcMeshTangent> Tangents;


	for (int32 i = 0; i < Sections; i++) {
		UKismetProceduralMeshLibrary::GetSectionFromStaticMesh(StaticMeshComponent->GetStaticMesh(), 0, i, CurrentSectionVertices, CurrentSectionTriangles, Normals, UV, Tangents);
		for (int32 Triangle : CurrentSectionTriangles) {
			TotalSectionsTriangles.Add(Triangle + TotalSectionsVertices.Num());
		}
		TotalSectionsVertices.Append(CurrentSectionVertices);
	}


	Meshes.Emplace(TotalSectionsVertices, TotalSectionsTriangles);

	return Meshes;
}

float TMathUtilities::SignedVolumeOfTriangle(const FVector& p1, const FVector& p2, const FVector& p3)
{
	float v321 = p3.X * p2.Y * p1.Z;
	float v231 = p2.X * p3.Y * p1.Z;
	float v312 = p3.X * p1.Y * p2.Z;
	float v132 = p1.X * p3.Y * p2.Z;
	float v213 = p2.X * p1.Y * p3.Z;
	float v123 = p1.X * p2.Y * p3.Z;

	return (1.0f / 6.0f) * (-v321 + v231 + v312 - v132 - v213 + v123);
}

// References:
// http://stackoverflow.com/questions/1406029/how-to-calculate-the-volume-of-a-3d-mesh-object-the-surface-of-which-is-made-up-t
// http://research.microsoft.com/en-us/um/people/chazhang/publications/icip01_ChaZhang.pdf
// TODO: Use local space for float precision.
float TMathUtilities::MeshVolume(UStaticMeshComponent* StaticMeshComponent)
{
	float Volume = 0.f;
	for (const auto& TriangleMesh : TMeshUtilities::GetTriangleMeshes(StaticMeshComponent))
	{
		const auto TriangleCount = TriangleMesh.TriangleVertexIndices.Num() / 3;
		for (int32 i = 0; i < TriangleCount; ++i)
		{
			const auto Vertex1 = TriangleMesh.Vertices[TriangleMesh.TriangleVertexIndices[i * 3 + 0]];
			const auto Vertex2 = TriangleMesh.Vertices[TriangleMesh.TriangleVertexIndices[i * 3 + 2]];
			const auto Vertex3 = TriangleMesh.Vertices[TriangleMesh.TriangleVertexIndices[i * 3 + 1]];

			const auto LocalToWorld = StaticMeshComponent->GetComponentTransform();

			const auto WorldVertex1 = LocalToWorld.TransformPosition(Vertex1);
			const auto WorldVertex2 = LocalToWorld.TransformPosition(Vertex2);
			const auto WorldVertex3 = LocalToWorld.TransformPosition(Vertex3);

			Volume += SignedVolumeOfTriangle(WorldVertex1, WorldVertex2, WorldVertex3);
		}
	}
	return FMath::Abs(Volume);
}
