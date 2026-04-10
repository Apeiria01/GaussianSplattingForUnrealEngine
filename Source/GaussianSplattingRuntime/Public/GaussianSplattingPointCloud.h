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
// Number of packed SH float4 slots per point in GPU SoA SHRest buffer
#define GS_GPU_SH_FLOAT4S_PER_POINT 12

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

	// SoA views aligned with SIBR-style field granularity.
	const TArray<FVector3f>& GetPositionsSOA() const { return PositionsSOA; }
	const TArray<FQuat4f>& GetQuatsSOA() const { return QuatsSOA; }
	const TArray<FVector3f>& GetScalesSOA() const { return ScalesSOA; }
	const TArray<FLinearColor>& GetColorsSOA() const { return ColorsSOA; }
	const TArray<int32>& GetSHDegreesSOA() const { return SHDegreesSOA; }
	const TArray<float>& GetSHCoeffsSOA() const { return SHCoeffsSOA; }

private:
	void Serialize(FArchive& Ar) override;
	void RebuildSoACache();

private:
	UPROPERTY(EditAnywhere, Category = "Gaussian Splatting")
	EGaussianSplattingCompressionMethod CompressionMethod = EGaussianSplattingCompressionMethod::Zlib;

	UPROPERTY(Transient)
	TArray<FGaussianSplattingPoint> Points;

	UPROPERTY()
	uint32 FeatureLevel = 64;

	// Runtime SoA cache rebuilt from Points.
	TArray<FVector3f> PositionsSOA;
	TArray<FQuat4f> QuatsSOA;
	TArray<FVector3f> ScalesSOA;
	TArray<FLinearColor> ColorsSOA;
	TArray<int32> SHDegreesSOA;
	TArray<float> SHCoeffsSOA;
};
