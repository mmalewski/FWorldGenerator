// This file is part of the FWorldGenenerator.
// Copyright Aleksandr "Flone" Tretyakov (github.com/Flone-dnb).
// Licensed under the ZLib license.
// Refer to the LICENSE file included.

#include "FWGen.h"

// STL
#include <ctime>

// External
#include "PerlinNoise.hpp"

#if WITH_EDITOR
#include "Components/BoxComponent.h"
#endif // WITH_EDITOR

#include "Components/StaticMeshComponent.h"

// --------------------------------------------------------------------------------------------------------
// --------------------------------------------------------------------------------------------------------
// --------------------------------------------------------------------------------------------------------



AFWGen::AFWGen()
{
	PrimaryActorTick.bCanEverTick = false;
	bWorldCreated                 = false;


	iGeneratedSeed                = 0;


	pChunkMap                     = nullptr;




	// Mesh structure

	pRootNode = CreateDefaultSubobject<USceneComponent>("Root");
	RootComponent = pRootNode;

	pProcMeshComponent = CreateDefaultSubobject<UProceduralMeshComponent>("ProcMeshComp");
	//pProcMeshComponent->bUseAsyncCooking = true;  // commented because the world should be generated after GenerateWorld() returned, and not calculate something after the function returned.
	pProcMeshComponent->RegisterComponent();
	pProcMeshComponent->SetupAttachment(RootComponent);

	// Enable collision
	pProcMeshComponent->ContainsPhysicsTriMeshData(true);



	pChunkMap = new FWGenChunkMap();




	// Water Plane

	WaterPlane = CreateDefaultSubobject<UStaticMeshComponent>("WaterPlane");
	WaterPlane->SetupAttachment(RootComponent);
	WaterPlane->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> PlaneMeshAsset(TEXT("StaticMesh'/Engine/BasicShapes/Plane.Plane'"));
	WaterPlane->SetStaticMesh(PlaneMeshAsset.Object);

	if (CreateWater == false) WaterPlane->SetVisibility(false);




	// Materials

	WaterMaterial  = nullptr;
	GroundMaterial = nullptr;



	// Preview Plane

#if WITH_EDITOR
	PreviewPlane = CreateDefaultSubobject<UBoxComponent>("PreviewPlane");
	PreviewPlane->SetupAttachment(RootComponent);
	PreviewPlane->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	refreshPreview();

	if (ComplexPreview) GenerateWorld();
#endif // WITH_EDITOR
}

AFWGen::~AFWGen()
{
	if (pChunkMap)
	{
		delete pChunkMap;
	}

	if (!pProcMeshComponent->IsValidLowLevel())
	{
		return;
	}

	if (pProcMeshComponent->IsPendingKill())
	{
return;
	}

	pProcMeshComponent->DestroyComponent();
}

void AFWGen::GenerateWorld()
{
	if (pChunkMap)
	{
		pChunkMap->clearWorld(pProcMeshComponent);
	}



	generateSeed();

	if (WorldSize != -1)
	{
		size_t iChunkCount = (ViewDistance * 2 + 1) * (ViewDistance * 2 + 1);
		std::vector<std::promise<bool>> vPromiseThreads(iChunkCount);
		std::vector<std::future<bool>> vFutureThreads;

		for (size_t i = 0; i < iChunkCount; i++)
		{
			vFutureThreads.push_back(vPromiseThreads[i].get_future());
		}


		// Generate the chunks.

		int32 iSectionIndex = 0;

		
		for (long long y = ViewDistance; y > -ViewDistance - 1; y--)
		{
			for (long long x = -ViewDistance; x < ViewDistance + 1; x++)
			{
				std::thread tGenerateChunk(&AFWGen::generateChunk, this, std::move(vPromiseThreads[iSectionIndex]), x, y, iSectionIndex);
				tGenerateChunk.detach();

				iSectionIndex++;
			}
		}

		for (size_t i = 0; i < vFutureThreads.size(); i++)
		{
			vFutureThreads[i].get();
		}

		std::sort(pChunkMap->vChunks.begin(), pChunkMap->vChunks.end(), [](FWGenChunk* chunk1, FWGenChunk* chunk2) -> bool
			{
				return chunk1->iSectionIndex < chunk2->iSectionIndex ? true : false;
			});

		for (size_t i = 0; i < pChunkMap->vChunks.size(); i++)
		{
			pProcMeshComponent->CreateMeshSection_LinearColor(pChunkMap->vChunks[i]->iSectionIndex, pChunkMap->vChunks[i]->vVertices, pChunkMap->vChunks[i]->vTriangles,
				pChunkMap->vChunks[i]->vNormals, pChunkMap->vChunks[i]->vUV0, pChunkMap->vChunks[i]->vVertexColors,
				pChunkMap->vChunks[i]->vTangents, true);

			// Set material
			if (GroundMaterial)
			{
				pProcMeshComponent->SetMaterial(pChunkMap->vChunks[i]->iSectionIndex, GroundMaterial);
			}

			pChunkMap->vChunks[i]->setMeshSection(pProcMeshComponent->GetProcMeshSection(pChunkMap->vChunks[i]->iSectionIndex));
		}
	}
	else
	{
		std::promise<bool> promise;
		std::future<bool> future = promise.get_future();

		std::thread tGenerateChunk(&AFWGen::generateChunk, this, std::move(promise), 0, 0, 0);
		tGenerateChunk.detach();

		future.get();
	}

	if (ApplyGroundMaterialBlend)
	{
		blendWorldMaterialsMore();
	}
	
	if (ApplySlopeDependentBlend)
	{
		applySlopeDependentBlend();
	}
	

	for (size_t i = 0; i < pChunkMap->vChunks.size(); i++)
	{
		pProcMeshComponent->UpdateMeshSection_LinearColor(i, pChunkMap->vChunks[i]->vVertices, pChunkMap->vChunks[i]->vNormals,
			pChunkMap->vChunks[i]->vUV0, pChunkMap->vChunks[i]->vVertexColors, pChunkMap->vChunks[i]->vTangents);
	}




	// Water Plane

	WaterPlane->SetWorldLocation(FVector(
		GetActorLocation().X,
		GetActorLocation().Y,
		GetActorLocation().Z + ((GenerationMaxZFromActorZ - GetActorLocation().Z) * ZWaterLevelInWorld)
	));

	WaterPlane->SetWorldScale3D(FVector(
		((ChunkPieceColumnCount)*ChunkPieceSizeX) * (WaterSize / 100.0f),
		((ChunkPieceRowCount)*ChunkPieceSizeY) * (WaterSize / 100.0f),
		0.1f
	));

	if (WaterMaterial)
	{
		WaterPlane->SetMaterial(0, WaterMaterial);
	}

	if (CreateWater)
	{
		WaterPlane->SetVisibility(true);
	}
	else
	{
		WaterPlane->SetVisibility(false);
	}
}

void AFWGen::blendWorldMaterialsMore()
{
	std::mt19937_64 gen(std::random_device{}());
	std::uniform_real_distribution<float> urd(0.0f, 1.0f);

	for (size_t i = 0; i < pChunkMap->vChunks.size(); i++)
	{
		size_t iVertexIndex = 0;

		for (size_t iRow = 0; iRow < ChunkPieceRowCount + 1; iRow++)
		{
			iVertexIndex = (ChunkPieceColumnCount + 1) * iRow;

			size_t iSkipPoints = 0;

			for (size_t iColumn = 0; iColumn < ChunkPieceColumnCount + 1; iColumn++)
			{
				if (iSkipPoints > 0)
				{
					iSkipPoints--;
					iVertexIndex++;

					continue;
				}

				if ((iRow >= 3) && ((iRow + 3) < (ChunkPieceRowCount + 1))
					&& (iColumn >= 3) && ((iColumn + 3) < (ChunkPieceColumnCount + 1)))
				{
					if (SecondMaterialUnderWater &&
						(pChunkMap->vChunks[i]->vVertices[iVertexIndex].Z <= (GetActorLocation().Z + (GenerationMaxZFromActorZ * (ZWaterLevelInWorld + 0.008f)))))
					{
						// Under water material - don't touch.
					}
					else
					{
						bool bFirstLayerWithOtherColor  = false;
						bool bSecondLayerWithOtherColor = false;
						bool bThirdLayerWithOtherColor  = false;

						if ((pChunkMap->vChunks[i]->vLayerIndex[iVertexIndex] == 1)
							&& (areEqual(pChunkMap->vChunks[i]->vVertexColors[iVertexIndex].A, 0.0f, 0.1f) == false))
						{
							bFirstLayerWithOtherColor = true;
						}
						else if ((pChunkMap->vChunks[i]->vLayerIndex[iVertexIndex] == 2)
							&& (areEqual(pChunkMap->vChunks[i]->vVertexColors[iVertexIndex].A, 0.5f, 0.1f) == false))
						{
							bSecondLayerWithOtherColor = true;
						}
						else if ((pChunkMap->vChunks[i]->vLayerIndex[iVertexIndex] == 3)
							&& (areEqual(pChunkMap->vChunks[i]->vVertexColors[iVertexIndex].A, 1.0f, 0.1f) == false))
						{
							bThirdLayerWithOtherColor = true;
						}


						if (bFirstLayerWithOtherColor || bSecondLayerWithOtherColor || bThirdLayerWithOtherColor)
						{
							// Add a little more of other color.

							// . . . . .
							// . . . . .
							// . . + . .  <- we are in the center.
							// . . . . .
							// . . . . .

							// Close square:

							for (int32 iIndexY = -(ChunkPieceColumnCount + 1); iIndexY <= (ChunkPieceColumnCount + 1); iIndexY += (ChunkPieceColumnCount + 1))
							{
								for (int32 iIndexX = -1; iIndexX <= 1; iIndexX++)
								{
									if (urd(gen) <= IncreasedMaterialBlendProbability)
									{
										pChunkMap->vChunks[i]->vVertexColors[iVertexIndex + iIndexY + iIndexX] = pChunkMap->vChunks[i]->vVertexColors[iVertexIndex];
									}
								}
							}

							// Far square:

							for (int32 iIndexY = -((ChunkPieceColumnCount + 1) * 2 - 1); iIndexY <= ((ChunkPieceColumnCount + 1) * 2 - 1); iIndexY += (ChunkPieceColumnCount + 1))
							{
								for (int32 iIndexX = -2; iIndexX <= 2; iIndexX++)
								{
									if (urd(gen) <= (IncreasedMaterialBlendProbability / 2))
									{
										pChunkMap->vChunks[i]->vVertexColors[iVertexIndex + iIndexY + iIndexX] = pChunkMap->vChunks[i]->vVertexColors[iVertexIndex];
									}
								}
							}

							iSkipPoints = 2;
						}
					}
				}
				
				iVertexIndex++;
			}
		}
	}
}

void AFWGen::applySlopeDependentBlend()
{
	float fSteepSlopeMinHeightDiff = GenerationMaxZFromActorZ * MinSlopeHeightMultiplier;

	for (size_t iChunk = 0; iChunk < pChunkMap->vChunks.size(); iChunk++)
	{
		std::vector<bool> vProcessedVertices(pChunkMap->vChunks[iChunk]->vVertices.Num());
		vProcessedVertices[0] = true;

		size_t iVertexIndex = 0;

		for (size_t iRow = 0; iRow < ChunkPieceRowCount + 1; iRow++)
		{
			for (size_t iColumn = 0; iColumn < ChunkPieceColumnCount + 1; iColumn++)
			{
				if (((iRow < 2) || (iRow > ChunkPieceRowCount - 3))
					|| ((iColumn < 2) || (iColumn > ChunkPieceColumnCount - 3)))
				{
					iVertexIndex++;
					continue;
				}

				bool bHasLeftPoints  = true;
				bool bHasTopPoints   = true;
				bool bHasRightPoints = true;
				bool bHasDownPoints  = true;

				if (iRow == 0)
				{
					bHasTopPoints = false;
				}

				if (iRow == ChunkPieceRowCount)
				{
					bHasDownPoints = false;
				}

				if (iColumn == 0)
				{
					bHasLeftPoints = false;
				}

				if (iColumn == ChunkPieceColumnCount)
				{
					bHasRightPoints = false;
				}

				float fCurrentVertexZ = pChunkMap->vChunks[iChunk]->vVertices[iVertexIndex].Z;

				// Process the left points:
				if (bHasLeftPoints)
				{
					compareHeightDifference(pChunkMap->vChunks[iChunk], vProcessedVertices, fCurrentVertexZ, iVertexIndex - 1, fSteepSlopeMinHeightDiff);

					if (bHasTopPoints)
					{
						compareHeightDifference(pChunkMap->vChunks[iChunk], vProcessedVertices, fCurrentVertexZ, iVertexIndex - 1 - (ChunkPieceColumnCount + 1), fSteepSlopeMinHeightDiff);
					}

					if (bHasDownPoints)
					{
						compareHeightDifference(pChunkMap->vChunks[iChunk], vProcessedVertices, fCurrentVertexZ, iVertexIndex - 1 + (ChunkPieceColumnCount + 1), fSteepSlopeMinHeightDiff);
					}
				}

				// Process the right points:
				if (bHasRightPoints)
				{
				compareHeightDifference(pChunkMap->vChunks[iChunk], vProcessedVertices, fCurrentVertexZ, iVertexIndex + 1, fSteepSlopeMinHeightDiff);

				if (bHasTopPoints)
				{
					compareHeightDifference(pChunkMap->vChunks[iChunk], vProcessedVertices, fCurrentVertexZ, iVertexIndex + 1 - (ChunkPieceColumnCount + 1), fSteepSlopeMinHeightDiff);
				}

				if (bHasDownPoints)
				{
					compareHeightDifference(pChunkMap->vChunks[iChunk], vProcessedVertices, fCurrentVertexZ, iVertexIndex + 1 + (ChunkPieceColumnCount + 1), fSteepSlopeMinHeightDiff);
				}
				}

				// Top point:
				if (bHasTopPoints)
				{
					compareHeightDifference(pChunkMap->vChunks[iChunk], vProcessedVertices, fCurrentVertexZ, iVertexIndex - (ChunkPieceColumnCount + 1), fSteepSlopeMinHeightDiff);
				}

				// Down point:
				if (bHasDownPoints)
				{
					compareHeightDifference(pChunkMap->vChunks[iChunk], vProcessedVertices, fCurrentVertexZ, iVertexIndex + (ChunkPieceColumnCount + 1), fSteepSlopeMinHeightDiff);
				}

				iVertexIndex++;
			}
		}
	}
}

#if WITH_EDITOR
void AFWGen::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	FName MemberPropertyChanged = (PropertyChangedEvent.MemberProperty ? PropertyChangedEvent.MemberProperty->GetFName() : NAME_None);
	

	if (
		MemberPropertyChanged == GET_MEMBER_NAME_CHECKED(AFWGen, ChunkPieceRowCount)
		|| MemberPropertyChanged == GET_MEMBER_NAME_CHECKED(AFWGen, ChunkPieceColumnCount)
		|| MemberPropertyChanged == GET_MEMBER_NAME_CHECKED(AFWGen, ChunkPieceSizeX)
		|| MemberPropertyChanged == GET_MEMBER_NAME_CHECKED(AFWGen, ChunkPieceSizeY)
		|| MemberPropertyChanged == GET_MEMBER_NAME_CHECKED(AFWGen, GenerationFrequency)
		|| MemberPropertyChanged == GET_MEMBER_NAME_CHECKED(AFWGen, GenerationOctaves)
		|| MemberPropertyChanged == GET_MEMBER_NAME_CHECKED(AFWGen, GenerationSeed)
		|| MemberPropertyChanged == GET_MEMBER_NAME_CHECKED(AFWGen, GenerationMaxZFromActorZ)
		|| MemberPropertyChanged == GET_MEMBER_NAME_CHECKED(AFWGen, ComplexPreview)
		|| MemberPropertyChanged == GET_MEMBER_NAME_CHECKED(AFWGen, InvertWorld)
		|| MemberPropertyChanged == GET_MEMBER_NAME_CHECKED(AFWGen, FirstMaterialOnSecondProbability)
		|| MemberPropertyChanged == GET_MEMBER_NAME_CHECKED(AFWGen, FirstMaterialOnThirdProbability)
		|| MemberPropertyChanged == GET_MEMBER_NAME_CHECKED(AFWGen, SecondMaterialOnFirstProbability)
		|| MemberPropertyChanged == GET_MEMBER_NAME_CHECKED(AFWGen, SecondMaterialOnThirdProbability)
		|| MemberPropertyChanged == GET_MEMBER_NAME_CHECKED(AFWGen, ThirdMaterialOnFirstProbability)
		|| MemberPropertyChanged == GET_MEMBER_NAME_CHECKED(AFWGen, ThirdMaterialOnSecondProbability)
		|| MemberPropertyChanged == GET_MEMBER_NAME_CHECKED(AFWGen, TerrainCutHeightFromActorZ)
		)
	{
		if ((TerrainCutHeightFromActorZ > 1.0f) || (TerrainCutHeightFromActorZ < 0.0f))
		{
			TerrainCutHeightFromActorZ = 1.0f;
		}

		if ((FirstMaterialOnSecondProbability < 0.0f) || (FirstMaterialOnSecondProbability > 1.0f))
		{
			FirstMaterialOnSecondProbability = 0.01f;
		}

		if ((FirstMaterialOnThirdProbability < 0.0f) || (FirstMaterialOnThirdProbability > 1.0f))
		{
			FirstMaterialOnThirdProbability = 0.01f;
		}

		if ((SecondMaterialOnFirstProbability < 0.0f) || (SecondMaterialOnFirstProbability > 1.0f))
		{
			SecondMaterialOnFirstProbability = 0.01f;
		}

		if ((SecondMaterialOnThirdProbability < 0.0f) || (SecondMaterialOnThirdProbability > 1.0f))
		{
			SecondMaterialOnThirdProbability = 0.01f;
		}

		if ((ThirdMaterialOnFirstProbability < 0.0f) || (ThirdMaterialOnFirstProbability > 1.0f))
		{
			ThirdMaterialOnFirstProbability = 0.01f;
		}

		if ((ThirdMaterialOnSecondProbability < 0.0f) || (ThirdMaterialOnSecondProbability > 1.0f))
		{
			ThirdMaterialOnSecondProbability = 0.01f;
		}

		if ((IncreasedMaterialBlendProbability < 0.0f) || (IncreasedMaterialBlendProbability > 1.0f))
		{
			IncreasedMaterialBlendProbability = 0.0f;
		}



		if (ChunkPieceRowCount < 1)
		{
			ChunkPieceRowCount = 1;
		}

		if (ChunkPieceColumnCount < 1)
		{
			ChunkPieceColumnCount = 1;
		}

		if (GenerationMaxZFromActorZ < 0.0f)
		{
			GenerationMaxZFromActorZ = 0.0f;
		}

		if (GenerationSeed < 0)
		{
			GenerationSeed = 0;
		}

		if (GenerationFrequency > 64.0f)
		{
			GenerationFrequency = 64.0f;
		}
		else if (GenerationFrequency < 0.1f)
		{
			GenerationFrequency = 0.1f;
		}

		if (GenerationOctaves > 16)
		{
			GenerationOctaves = 16;
		}
		else if (GenerationOctaves < 1)
		{
			GenerationOctaves = 1;
		}

		refreshPreview();

		if (ComplexPreview)
		{
			GenerateWorld();
		}
		else
		{
			pChunkMap->clearWorld(pProcMeshComponent);
		}
	}
	else if ( MemberPropertyChanged == GET_MEMBER_NAME_CHECKED(AFWGen, CreateWater) 
			|| MemberPropertyChanged == GET_MEMBER_NAME_CHECKED(AFWGen, ZWaterLevelInWorld)
			|| MemberPropertyChanged == GET_MEMBER_NAME_CHECKED(AFWGen, WaterSize)
			|| MemberPropertyChanged == GET_MEMBER_NAME_CHECKED(AFWGen, WaterMaterial)
			|| MemberPropertyChanged == GET_MEMBER_NAME_CHECKED(AFWGen, GroundMaterial))
	{
		if (ZWaterLevelInWorld < 0.0f)
		{
			ZWaterLevelInWorld = 0.0f;
		}
		else if (ZWaterLevelInWorld > 1.0f)
		{
			ZWaterLevelInWorld = 1.0f;
		}

		if (GroundMaterial)
		{
			pProcMeshComponent->SetMaterial(0, GroundMaterial);
		}

		if (CreateWater)
		{
			if (WaterMaterial)
			{
				WaterPlane->SetMaterial(0, WaterMaterial);
				WaterPlane->SetVisibility(true);
			}

			WaterPlane->SetWorldLocation(FVector(
				GetActorLocation().X,
				GetActorLocation().Y,
				GetActorLocation().Z + ((GenerationMaxZFromActorZ - GetActorLocation().Z) * ZWaterLevelInWorld)
			));
			WaterPlane->SetWorldScale3D(FVector(
				((ChunkPieceColumnCount) * ChunkPieceSizeX) * (WaterSize / 100.0f),
				((ChunkPieceRowCount) * ChunkPieceSizeY)    * (WaterSize / 100.0f),
				0.1f
			));
		}
		else
		{
			WaterPlane->SetVisibility(false);
		}
	}
	else if ((MemberPropertyChanged == GET_MEMBER_NAME_CHECKED(AFWGen, ViewDistance))
			||
			(MemberPropertyChanged == GET_MEMBER_NAME_CHECKED(AFWGen, WorldSize)))
	{
		if (WorldSize < -1)
		{
			WorldSize = -1;
		}

		if (ViewDistance < 1)
		{
			ViewDistance = 1;
		}

		refreshPreview();

		if (ComplexPreview) GenerateWorld();
	}
	else
	{
		refreshPreview();

		if (ComplexPreview) GenerateWorld();
	}
}
#endif // WITH_EDITOR

bool AFWGen::SetChunkPieceRowCount(int32 NewChunkPieceRowCount)
{
	if (NewChunkPieceRowCount < 1)
	{
		return true;
	}
	else
	{
		ChunkPieceRowCount = NewChunkPieceRowCount;

		return false;
	}
}

bool AFWGen::SetChunkPieceColumnCount(int32 NewChunkPieceColumnCount)
{
	if (NewChunkPieceColumnCount < 1)
	{
		return true;
	}
	else
	{
		ChunkPieceColumnCount = NewChunkPieceColumnCount;

		return false;
	}
}

bool AFWGen::SetChunkPieceSizeX(float NewChunkPieceSizeX)
{
	if (NewChunkPieceSizeX < 0.5f)
	{
		return true;
	}
	else
	{
		ChunkPieceSizeX = NewChunkPieceSizeX;

		return false;
	}
}

bool AFWGen::SetChunkPieceSizeY(float NewChunkPieceSizeY)
{
	if (NewChunkPieceSizeY < 0.5f)
	{
		return true;
	}
	else
	{
		ChunkPieceSizeY = NewChunkPieceSizeY;

		return false;
	}
}

bool AFWGen::SetViewDistance(int32 NewViewDistance)
{
	if (NewViewDistance < 1)
	{
		return true;
	}
	else
	{
		ViewDistance = NewViewDistance;

		return false;
	}
}

bool AFWGen::SetGenerationFrequency(float NewGenerationFrequency)
{
	if ((NewGenerationFrequency > 64.0f) || (NewGenerationFrequency < 0.1f))
	{
		return true;
	}
	else
	{
		GenerationFrequency = NewGenerationFrequency;

		return false;
	}
}

bool AFWGen::SetGenerationOctaves(int32 NewGenerationOctaves)
{
	if ((NewGenerationOctaves > 16) || (NewGenerationOctaves < 1))
	{
		return true;
	}
	else
	{
		GenerationOctaves = NewGenerationOctaves;

		return false;
	}
}

bool AFWGen::SetGenerationSeed(int32 NewGenerationSeed)
{
	if (NewGenerationSeed < 0)
	{
		return true;
	}
	else
	{
		GenerationSeed = NewGenerationSeed;

		return false;
	}
}

bool AFWGen::SetGenerationMaxZFromActorZ(float NewGenerationMaxZFromActorZ)
{
	if (NewGenerationMaxZFromActorZ < 0.0f)
	{
		return true;
	}
	else
	{
		GenerationMaxZFromActorZ = NewGenerationMaxZFromActorZ;

		return false;
	}
}

void AFWGen::SetInvertWorld(bool InvertWorld)
{
	this->InvertWorld = InvertWorld;
}

bool AFWGen::SetWorldSize(int32 NewWorldSize)
{
	if (NewWorldSize < -1)
	{
		return true;
	}
	else
	{
		WorldSize = NewWorldSize;

		return false;
	}
}

void AFWGen::SetGroundMaterial(UMaterialInterface* NewGroundMaterial)
{
	GroundMaterial = NewGroundMaterial;
}

bool AFWGen::SetFirstMaterialMaxRelativeHeight(float NewFirstMaterialMaxRelativeHeight)
{
	if ((NewFirstMaterialMaxRelativeHeight < 0.0f) || (NewFirstMaterialMaxRelativeHeight > 1.0f) || (NewFirstMaterialMaxRelativeHeight > SecondMaterialMaxRelativeHeight))
	{
		return true;
	}
	else
	{
		FirstMaterialMaxRelativeHeight = NewFirstMaterialMaxRelativeHeight;

		return false;
	}
}

bool AFWGen::SetSecondMaterialMaxRelativeHeight(float NewSecondMaterialMaxRelativeHeight)
{
	if ((NewSecondMaterialMaxRelativeHeight < 0.0f) || (NewSecondMaterialMaxRelativeHeight > 1.0f) || (NewSecondMaterialMaxRelativeHeight < FirstMaterialMaxRelativeHeight))
	{
		return true;
	}
	else
	{
		SecondMaterialMaxRelativeHeight = NewSecondMaterialMaxRelativeHeight;

		return false;
	}
}

bool AFWGen::SetMaterialHeightMaxDeviation(float NewMaterialHeightMaxDeviation)
{
	if ((NewMaterialHeightMaxDeviation > 1.0f) || (NewMaterialHeightMaxDeviation < 0.0f))
	{
		return true;
	}
	else
	{
		MaterialHeightMaxDeviation = NewMaterialHeightMaxDeviation;

		return false;
	}
}

bool AFWGen::SetTerrainCutHeightFromActorZ(float NewTerrainCutHeightFromActorZ)
{
	if ((NewTerrainCutHeightFromActorZ > 1.0f) || (NewTerrainCutHeightFromActorZ < 0.0f))
	{
		return true;
	}
	else
	{
		TerrainCutHeightFromActorZ = NewTerrainCutHeightFromActorZ;
		return false;
	}
}

bool AFWGen::SetFirstMaterialOnOtherProbability(float FirstOnSecond, float FirstOnThird)
{
	if ( ((FirstOnSecond > 1.0f) || (FirstOnSecond < 0.0f)) || ((FirstOnThird > 1.0f) || (FirstOnThird < 0.0f)))
	{
		return true;
	}
	else
	{
		FirstMaterialOnSecondProbability = FirstOnSecond;
		FirstMaterialOnThirdProbability  = FirstOnThird;
		return false;
	}
}

bool AFWGen::SetSecondMaterialOnOtherProbability(float SecondOnFirst, float SecondOnThird)
{
	if ( ((SecondOnFirst > 1.0f) || (SecondOnFirst < 0.0f)) || ((SecondOnThird > 1.0f) || (SecondOnThird < 0.0f)))
	{
		return true;
	}
	else
	{
		SecondMaterialOnFirstProbability = SecondOnFirst;
		SecondMaterialOnThirdProbability = SecondOnThird;
		return false;
	}
}

bool AFWGen::SetThirdMaterialOnOtherProbability(float ThirdOnFirst, float ThirdOnSecond)
{
	if ( ((ThirdOnFirst > 1.0f) || (ThirdOnFirst < 0.0f)) || ((ThirdOnSecond > 1.0f) || (ThirdOnSecond < 0.0f)))
	{
		return true;
	}
	else
	{
		ThirdMaterialOnFirstProbability  = ThirdOnFirst;
		ThirdMaterialOnSecondProbability = ThirdOnSecond;
		return false;
	}
}

void AFWGen::SetCreateWater(bool CreateWater)
{
	this->CreateWater = CreateWater;
}

bool AFWGen::SetZWaterLevelInWorld(float NewZWaterLevelInWorld)
{
	if ((NewZWaterLevelInWorld < 0.0f) || (NewZWaterLevelInWorld > 1.0f))
	{
		return true;
	}
	else
	{
		ZWaterLevelInWorld = NewZWaterLevelInWorld;

		return false;
	}
}

bool AFWGen::SetWaterSize(int32 NewWaterSize)
{
	if (NewWaterSize <= 0)
	{
		return true;
	}
	else
	{
		WaterSize = NewWaterSize;

		return false;
	}
}

void AFWGen::SetWaterMaterial(UMaterialInterface* NewWaterMaterial)
{
	WaterMaterial = NewWaterMaterial;
}

#if WITH_EDITOR
void AFWGen::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);

	refreshPreview();

	if (ComplexPreview) GenerateWorld();
}
#endif // WITH_EDITOR

void AFWGen::BeginPlay()
{
	Super::BeginPlay();
}

void AFWGen::generateSeed()
{
	uint32_t seed = 0;

	if (GenerationSeed == 0)
	{
		// Generate seed
		std::mt19937_64 gen(std::random_device{}());
		std::uniform_int_distribution<unsigned int> uid(0, UINT_MAX);

		seed = uid(gen);
	}
	else
	{
		seed = GenerationSeed;
	}

	iGeneratedSeed = seed;
}

float AFWGen::pickVertexMaterial(double height, bool bApplyRND, std::uniform_real_distribution<float>* pUrd, std::mt19937_64* pRnd, float* pfLayerTypeWithoutRnd)
{
	std::uniform_real_distribution<float> urd_mat(0.005f, 1.0f);
	std::uniform_int_distribution<int> urd_bool(0, 1);


	float fDiviation = (*pUrd)(*pRnd);

	float fVertexColor = 0.0f; // apply first material to the vertex

	if ( (height >= (FirstMaterialMaxRelativeHeight + fDiviation)) 
		&& (height <= (SecondMaterialMaxRelativeHeight + fDiviation)) )
	{
		fVertexColor = 0.5f; // apply second material to the vertex

		if (pfLayerTypeWithoutRnd)
		{
			*pfLayerTypeWithoutRnd = fVertexColor;
		}

		if (bApplyRND)
		{
			float fFirstProb = urd_mat(*pRnd);
			float fThirdProb  = urd_mat(*pRnd);

			bool bPickFirst = false, bPickThird = false;

			if (fFirstProb <= FirstMaterialOnSecondProbability)
			{
				bPickFirst = true;
			}

			if (fThirdProb <= ThirdMaterialOnSecondProbability)
			{
				bPickThird = true;
			}

			if (bPickFirst && bPickThird)
			{
				if (urd_bool(*pRnd))
				{
					// Pick Third
					fVertexColor = 1.0f; // apply third material to the vertex
				}
				else
				{
					// Pick First
					fVertexColor = 0.0f; // apply first material to the vertex
				}
			}
			else
			{
				if (bPickFirst)
				{
					fVertexColor = 0.0f; // apply first material to the vertex
				}

				if (bPickThird)
				{
					fVertexColor = 1.0f; // apply third material to the vertex
				}
			}
		}
	}
	else if (height >= (SecondMaterialMaxRelativeHeight + fDiviation))
	{
		fVertexColor = 1.0f; // apply third material to the vertex

		if (pfLayerTypeWithoutRnd)
		{
			*pfLayerTypeWithoutRnd = fVertexColor;
		}

		if (bApplyRND)
		{
			float fFirstProb  = urd_mat(*pRnd);
			float fSecondProb = urd_mat(*pRnd);

			bool bPickFirst = false, bPickSecond = false;

			if (fFirstProb <= FirstMaterialOnThirdProbability)
			{
				bPickFirst = true;
			}

			if (fSecondProb <= SecondMaterialOnThirdProbability)
			{
				bPickSecond = true;
			}

			if (bPickFirst && bPickSecond)
			{
				if (urd_bool(*pRnd))
				{
					// Pick Third
					fVertexColor = 0.5f; // apply second material to the vertex
				}
				else
				{
					// Pick First
					fVertexColor = 0.0f; // apply first material to the vertex
				}
			}
			else
			{
				if (bPickFirst)
				{
					fVertexColor = 0.0f; // apply first material to the vertex
				}

				if (bPickSecond)
				{
					fVertexColor = 0.5f; // apply second material to the vertex
				}
			}
		}
	}
	else if (bApplyRND)
	{
		// first material

		if (pfLayerTypeWithoutRnd)
		{
			*pfLayerTypeWithoutRnd = fVertexColor;
		}

		float fSecondProb = urd_mat(*pRnd);
		float fThirdProb  = urd_mat(*pRnd);

		bool bPickSecond = false, bPickThird = false;

		if (fSecondProb <= SecondMaterialOnFirstProbability)
		{
			bPickSecond = true;
		}

		if (fThirdProb <= ThirdMaterialOnFirstProbability)
		{
			bPickThird = true;
		}

		if (bPickSecond && bPickThird)
		{
			if (urd_bool(*pRnd))
			{
				// Pick Third
				fVertexColor = 1.0f; // apply third material to the vertex
			}
			else
			{
				// Pick Second
				fVertexColor = 0.5f; // apply second material to the vertex
			}
		}
		else
		{
			if (bPickSecond)
			{
				fVertexColor = 0.5f; // apply second material to the vertex
			}

			if (bPickThird)
			{
				fVertexColor = 1.0f; // apply third material to the vertex
			}
		}
	}

	return fVertexColor;
}

void AFWGen::generateChunk(std::promise<bool>&& promise, long long iX, long long iY, int32 iSectionIndex)
{
	// Generation setup

	uint32_t seed = iGeneratedSeed;



	// We ++ here because we start to make polygons from 2nd row
	int32 iCorrectedRowCount = ChunkPieceRowCount + 1;
	int32 iCorrectedColumnCount = ChunkPieceColumnCount + 1;




	// Perlin Noise setup

	const siv::PerlinNoise perlinNoise(seed);
	const double fx = ((iCorrectedColumnCount - 1) * ChunkPieceSizeX) / GenerationFrequency;
	const double fy = ((iCorrectedRowCount - 1) * ChunkPieceSizeY) / GenerationFrequency;




	// Prepare chunk coordinates

	float fChunkX = GetActorLocation().X;
	float fChunkY = GetActorLocation().Y;

	if (iX != 0)
	{
		// Left or right chunk

		fChunkX += (iX * ChunkPieceColumnCount * ChunkPieceSizeX);
	}
	
	if (iY != 0)
	{
		// Top or bottom chunk

		fChunkY += (iY * ChunkPieceRowCount * ChunkPieceSizeY);
	}




	// Generation params

	float fStartX = fChunkX - ((iCorrectedColumnCount - 1) * ChunkPieceSizeX) / 2;
	float fStartY = fChunkY - ((iCorrectedRowCount    - 1) * ChunkPieceSizeY) / 2;

	FVector vPrevLocation(fStartX, fStartY, GetActorLocation().Z);




	// Create chunk

	FWGenChunk* pNewChunk = new FWGenChunk(iX, iY, iSectionIndex);



	// Prepare random for vectex color

	std::mt19937_64 rnd(iGeneratedSeed);
	std::uniform_real_distribution<float> urd(-MaterialHeightMaxDeviation, MaterialHeightMaxDeviation);


	// Generation

	float fMaxGeneratedZ = GetActorLocation().Z;
	size_t iMaxGeneratedZIndex = 0;

	for (int32 i = 0; i < iCorrectedRowCount; i++)
	{
		for (int32 j = 0; j < iCorrectedColumnCount; j++)
		{
			// Generate vertex
			double generatedValue = perlinNoise.octaveNoise0_1(vPrevLocation.X / fx, vPrevLocation.Y / fy, GenerationOctaves);

			if (generatedValue > TerrainCutHeightFromActorZ)
			{
				generatedValue = TerrainCutHeightFromActorZ;
			}

			if (InvertWorld)
			{
				generatedValue = 1.0 - generatedValue;
			}


			pNewChunk->vNormals      .Add(FVector(0, 0, 1.0f));
			pNewChunk->vUV0          .Add(FVector2D(i, j));
			pNewChunk->vTangents     .Add(FProcMeshTangent(0.0f, 1.0f, 0.0f));



			// Set vertex Z

			// Here return value from perlinNoise.octaveNoise0_1 can be
			// 0    , but should be   GetActorLocation().Z
			// ...  , but should be   value from interval [GetActorLocation().Z; GenerationMaxZFromActorZ]
			// 1    , but should be   GenerationMaxZFromActorZ

			vPrevLocation.Z = GetActorLocation().Z + (GenerationMaxZFromActorZ * generatedValue);

			if (vPrevLocation.Z > fMaxGeneratedZ)
			{
				fMaxGeneratedZ = vPrevLocation.Z;
				iMaxGeneratedZIndex = pNewChunk->vVertices.Num();
			}

			pNewChunk->vVertices .Add (vPrevLocation);



			// Set "material" to vertex

			float fAlphaColor = 0.0f;

			float fAlphaColorWithoutRnd = 0.0f;

			if (SecondMaterialUnderWater && (generatedValue < (ZWaterLevelInWorld + 0.005f)))
			{
				fAlphaColor = 0.5f;
				fAlphaColorWithoutRnd = 0.5f;
			}
			else
			{
				if ((i == 0) || (i == iCorrectedRowCount - 1) || (j == 0)|| (j == iCorrectedColumnCount - 1))
				{
					fAlphaColor = pickVertexMaterial(generatedValue, false, &urd, &rnd, &fAlphaColorWithoutRnd);
				}
				else
				{
					fAlphaColor = pickVertexMaterial(generatedValue, true, &urd, &rnd, &fAlphaColorWithoutRnd);
				}
			}

			pNewChunk->vVertexColors .Add(FLinearColor(0.0f, 0.0f, 0.0f, fAlphaColor));

			if (areEqual(fAlphaColorWithoutRnd, 0.0f, 0.1f))
			{
				// First Layer

				pNewChunk->vLayerIndex.push_back(1);
			}
			else if (areEqual(fAlphaColorWithoutRnd, 0.5f, 0.1f))
			{
				// Second Layer
				pNewChunk->vLayerIndex.push_back(2);
			}
			else
			{
				// Third Layer
				pNewChunk->vLayerIndex.push_back(3);
			}



			vPrevLocation.X += ChunkPieceSizeX;

			if (i != 0)
			{
				//     j = 0,   1,   2,   3  ...
				// i = 0:  +----+----+----+- ...
				//         |   /|   /|   /|
				//         |  / |  / |  / |
				//         | /  | /  | /  |
				// i = 1:  +----+----+----+- ...

				int32 iFirstIndexInRow = (i - 1) * iCorrectedColumnCount;

				if (j == 0)
				{
					// Add triangle #1
					pNewChunk->vTriangles.Add(iFirstIndexInRow + j);
					pNewChunk->vTriangles.Add(i * iCorrectedColumnCount + j);
					pNewChunk->vTriangles.Add(iFirstIndexInRow + j + 1);
				}
				else
				{
					// Add triangle #2
					pNewChunk->vTriangles.Add(iFirstIndexInRow + j);
					pNewChunk->vTriangles.Add(i * iCorrectedColumnCount + j - 1);
					pNewChunk->vTriangles.Add(i * iCorrectedColumnCount + j);

					if (j < (iCorrectedColumnCount - 1))
					{
						// Add triangle #1
						pNewChunk->vTriangles.Add(iFirstIndexInRow + j);
						pNewChunk->vTriangles.Add(i * iCorrectedColumnCount + j);
						pNewChunk->vTriangles.Add(iFirstIndexInRow + j + 1);
					}
				}
			}
		}

		vPrevLocation.Set(fStartX, vPrevLocation.Y + ChunkPieceSizeY, GetActorLocation().Z);
	}

	pNewChunk->iMaxZVertexIndex = iMaxGeneratedZIndex;

	//pProcMeshComponent->CreateMeshSection_LinearColor(iSectionIndex, pNewChunk->vVertices, pNewChunk->vTriangles, pNewChunk->vNormals,
	//	pNewChunk->vUV0, pNewChunk->vVertexColors, pNewChunk->vTangents, true);

	//// Set material
	//if (GroundMaterial)
	//{
	//	pProcMeshComponent->SetMaterial(iSectionIndex, GroundMaterial);
	//}

	//pNewChunk->setMeshSection(pProcMeshComponent->GetProcMeshSection(iSectionIndex));

	pChunkMap->addChunk(pNewChunk);

	promise.set_value(true);
}

#if WITH_EDITOR
void AFWGen::refreshPreview()
{
	if (WorldSize == -1)
	{
		PreviewPlane ->SetBoxExtent ( FVector (
			(ChunkPieceColumnCount * ChunkPieceSizeX / 2),
			(ChunkPieceRowCount * ChunkPieceSizeY / 2),
			GenerationMaxZFromActorZ / 2
		)
		);
	}
	else if (WorldSize == 0)
	{
		PreviewPlane ->SetBoxExtent ( FVector (
			(ViewDistance * 2 + 1) * (ChunkPieceColumnCount * ChunkPieceSizeX / 2),
			(ViewDistance * 2 + 1) * (ChunkPieceRowCount * ChunkPieceSizeY / 2),
			GenerationMaxZFromActorZ / 2
		)
		);
	}
	else
	{
		PreviewPlane ->SetBoxExtent ( FVector (
			((WorldSize * ViewDistance) * 2 + 1) * (ChunkPieceColumnCount * ChunkPieceSizeX / 2),
			((WorldSize * ViewDistance) * 2 + 1) * (ChunkPieceRowCount * ChunkPieceSizeY / 2),
			GenerationMaxZFromActorZ / 2
		)
		);
	}

	PreviewPlane ->SetWorldLocation ( FVector (
		GetActorLocation () .X,
		GetActorLocation () .Y,
		GetActorLocation () .Z + (GenerationMaxZFromActorZ) / 2
	)   
	);
}
#endif // WITH_EDITOR

bool AFWGen::areEqual(float a, float b, float eps)
{
	return fabs(a - b) < eps;
}


void AFWGen::compareHeightDifference(FWGenChunk* pChunk, std::vector<bool>& vProcessedVertices, float& fCurrentZ, size_t iCompareToIndex, float& fSteepSlopeMinHeightDiff)
{
	if (vProcessedVertices[iCompareToIndex] == false)
	{
		if (fabs(pChunk->vVertices[iCompareToIndex].Z - fCurrentZ) > fSteepSlopeMinHeightDiff)
		{
			pChunk->vVertexColors[iCompareToIndex] = FLinearColor(0.0f, 0.0f, 0.0f, 0.5f);
		}

		vProcessedVertices[iCompareToIndex] = true;
	}
}

void AFWGen::SetSecondMaterialUnderWater(bool NewSecondMaterialUnderWater)
{
	SecondMaterialUnderWater = NewSecondMaterialUnderWater;
}

bool AFWGen::SetIncreasedMaterialBlendProbability(float NewIncreasedMaterialBlendProbability)
{
	if (NewIncreasedMaterialBlendProbability < 0.0f || NewIncreasedMaterialBlendProbability > 1.0f)
	{
		return true;
	}
	else
	{
		IncreasedMaterialBlendProbability = NewIncreasedMaterialBlendProbability;

		return false;
	}
}

void AFWGen::SetApplyGroundMaterialBlend(bool bApply)
{
	ApplyGroundMaterialBlend = bApply;
}

void AFWGen::SetApplySlopeDependentBlend(bool bApply)
{
	ApplySlopeDependentBlend = bApply;
}

bool AFWGen::SetMinSlopeHeightMultiplier(float NewMinSlopeHeightMultiplier)
{
	if (NewMinSlopeHeightMultiplier < 0.0f || NewMinSlopeHeightMultiplier > 1.0f)
	{
		return true;
	}
	else
	{
		MinSlopeHeightMultiplier = NewMinSlopeHeightMultiplier;

		return false;
	}
}

