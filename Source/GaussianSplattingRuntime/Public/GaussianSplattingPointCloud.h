#pragma once

#include "UObject/NoExportTypes.h"
#include "NiagaraDataInterfaceCurve.h"
#include "GaussianSplattingPointCloud.generated.h"


UENUM() 
enum class EGaussianSplattingCompressionMethod : uint8
{
	None,
	Zlib,
};

// Number of SH rest coefficients: 15 basis functions x 3 color channels = 45
#define GS_SH_REST_COUNT 45
// Number of float4 slots per point in GPU buffer: 4 (base) + 12 (SH) = 16
#define GS_GPU_FLOAT4S_PER_POINT 16

USTRUCT(BlueprintType, meta = (DisplayName = "Gaussian Splatting Point"))
struct GAUSSIANSPLATTINGRUNTIME_API FGaussianSplattingPoint
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Gaussian Splatting")
	FVector3f Position;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Gaussian Splatting")
	FQuat4f Quat;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Gaussian Splatting")
	FVector3f Scale;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Gaussian Splatting")
	FLinearColor Color;

	// Spherical Harmonics rest coefficients (f_rest_0..44)
	// Layout: interleaved [basis0_R, basis0_G, basis0_B, basis1_R, basis1_G, basis1_B, ...]
	float SHCoeffs[GS_SH_REST_COUNT];

	// SH degree: 0=DC only, 1=+3 basis, 2=+8 basis, 3=+15 basis (full)
	int32 SHDegree;

	FGaussianSplattingPoint(FVector3f InPos = FVector3f::ZeroVector, FQuat4f InQuat = FQuat4f::Identity, FVector3f InScale = {1,1,1}, FLinearColor InColor = FLinearColor::Black);

	bool operator==(const FGaussianSplattingPoint& Other)const;
	bool operator!=(const FGaussianSplattingPoint& Other)const;
	bool operator<(const FGaussianSplattingPoint& Other)const;

	friend FORCEINLINE uint32 GetTypeHash(const FGaussianSplattingPoint& ID)
	{
		return HashCombine(HashCombine(HashCombine(GetTypeHash(ID.Position), GetTypeHash(ID.Quat)), GetTypeHash(ID.Scale)), GetTypeHash(ID.Color));
	}
	friend FORCEINLINE FArchive& operator<<(FArchive& Ar, FGaussianSplattingPoint& Point)
	{
		Ar << Point.Position;
		Ar << Point.Quat;
		Ar << Point.Scale;
		Ar << Point.Color;
		// Serialize SH data
		for (int32 i = 0; i < GS_SH_REST_COUNT; i++)
		{
			Ar << Point.SHCoeffs[i];
		}
		Ar << Point.SHDegree;
		return Ar;
	}
};

UCLASS(Blueprintable, BlueprintType, EditInlineNew, CollapseCategories)
class GAUSSIANSPLATTINGRUNTIME_API UGaussianSplattingPointCloud : public UObject {
	GENERATED_UCLASS_BODY()
public:
	FSimpleMulticastDelegate OnPointsChanged;

	FRichCurve CalcFeatureCurve();

	FBox CalcBounds();

	void SetPoints(const TArray<FGaussianSplattingPoint>& InPoints, bool bReorder = true);

	const TArray<FGaussianSplattingPoint>& GetPoints() const;

	int32 GetPointCount() const;

	void LoadFromFile(FString InFilePath);

	static TArray<FGaussianSplattingPoint> LoadPointsFromFile(FString InFilePath);

	EGaussianSplattingCompressionMethod GetCompressionMethod() const { return CompressionMethod; }

	void SetCompressionMethod(EGaussianSplattingCompressionMethod val) { CompressionMethod = val; }

private:
	void Serialize(FArchive& Ar) override;

private:
	UPROPERTY(EditAnywhere, Category = "Gaussian Splatting")
	EGaussianSplattingCompressionMethod CompressionMethod = EGaussianSplattingCompressionMethod::Zlib;

	UPROPERTY(Transient)
	TArray<FGaussianSplattingPoint> Points;

	UPROPERTY()
	uint32 FeatureLevel = 64;
};
