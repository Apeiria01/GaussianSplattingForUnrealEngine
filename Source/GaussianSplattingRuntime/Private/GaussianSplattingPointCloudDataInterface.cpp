#include "GaussianSplattingPointCloudDataInterface.h"
#include "NiagaraCompileHashVisitor.h"
#include "NiagaraShaderParametersBuilder.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraRenderer.h"

#define LOCTEXT_NAMESPACE "GaussianSplatting"

static TAutoConsoleVariable<float> CVarGaussianSplattingScreenSizeBias(
	TEXT("r.GaussianSplatting.ScreenSizeBias"),
	0.0f,
	TEXT(""),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<float> CVarGaussianSplattingMaxFeatureSize(
	TEXT("r.GaussianSplatting.MaxFeatureSize"),
	0.0f,
	TEXT(""),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<float> CVarGaussianSplattingScreenSizeScale(
	TEXT("r.GaussianSplatting.ScreenSizeScale"),
	1.0f,
	TEXT(""),
	ECVF_RenderThreadSafe | ECVF_Scalability);

FNiagaraDataInterfaceProxyGaussianSplattingPointCloud::FNiagaraDataInterfaceProxyGaussianSplattingPointCloud(class UNiagaraDataInterfaceGaussianSplattingPointCloud* InOwner)
	: Owner(InOwner)
{

}

FNiagaraDataInterfaceProxyGaussianSplattingPointCloud::~FNiagaraDataInterfaceProxyGaussianSplattingPointCloud()
{
	if (PositionDegreeBuffer.NumBytes > 0)
	{
		PositionDegreeBuffer.Release();
	}
	if (RotationBuffer.NumBytes > 0)
	{
		RotationBuffer.Release();
	}
	if (ScaleOpacityBuffer.NumBytes > 0)
	{
		ScaleOpacityBuffer.Release();
	}
	if (ColorBuffer.NumBytes > 0)
	{
		ColorBuffer.Release();
	}
	if (SHRestBuffer.NumBytes > 0)
	{
		SHRestBuffer.Release();
	}
}
void FNiagaraDataInterfaceProxyGaussianSplattingPointCloud::MakeBufferDirty()
{
	bDirty = true;
}

void FNiagaraDataInterfaceProxyGaussianSplattingPointCloud::TryUpdateBuffer()
{
	if (PointCloud != Owner->PointCloud) {
		PointCloud = Owner->PointCloud;
		if (PointCloud) {
			PointCloud->OnPointsChanged.AddLambda([this]() {
				bDirty = true;
			});
		}
		bDirty = true;
	}
	if (bDirty) {
		PostDataToGPU();
		bDirty = false;
	}
}

void FNiagaraDataInterfaceProxyGaussianSplattingPointCloud::PostDataToGPU()
{
	if (Owner == nullptr || Owner->PointCloud == nullptr)
	{
		return;
	}

	const UGaussianSplattingPointCloud* SourcePointCloud = Owner->PointCloud;
	const int32 NumPoints = SourcePointCloud->GetPointCount();
	const TArray<FVector3f>& Positions = SourcePointCloud->GetPositionsSOA();
	const TArray<FQuat4f>& Quats = SourcePointCloud->GetQuatsSOA();
	const TArray<FVector3f>& Scales = SourcePointCloud->GetScalesSOA();
	const TArray<FLinearColor>& Colors = SourcePointCloud->GetColorsSOA();
	const TArray<int32>& SHDegrees = SourcePointCloud->GetSHDegreesSOA();
	const TArray<float>& SHCoeffs = SourcePointCloud->GetSHCoeffsSOA();

	if (Positions.Num() != NumPoints ||
		Quats.Num() != NumPoints ||
		Scales.Num() != NumPoints ||
		Colors.Num() != NumPoints ||
		SHDegrees.Num() != NumPoints ||
		SHCoeffs.Num() != NumPoints * GS_SH_REST_COUNT)
	{
		return;
	}

	TArray<FVector4f> PositionDegreeData;
	TArray<FVector4f> RotationData;
	TArray<FVector4f> ScaleOpacityData;
	TArray<FVector4f> ColorData;
	TArray<FVector4f> SHRestData;

	PositionDegreeData.SetNumUninitialized(NumPoints);
	RotationData.SetNumUninitialized(NumPoints);
	ScaleOpacityData.SetNumUninitialized(NumPoints);
	ColorData.SetNumUninitialized(NumPoints);
	SHRestData.SetNumUninitialized(NumPoints * GS_GPU_SH_FLOAT4S_PER_POINT);

	for (int32 PointIndex = 0; PointIndex < NumPoints; ++PointIndex)
	{
		const FVector3f& Position = Positions[PointIndex];
		const FQuat4f& Quat = Quats[PointIndex];
		const FVector3f& Scale = Scales[PointIndex];
		const FLinearColor& Color = Colors[PointIndex];
		const int32 SHDegree = SHDegrees[PointIndex];

		PositionDegreeData[PointIndex] = FVector4f(Position.X, Position.Y, Position.Z, (float)SHDegree);
		RotationData[PointIndex] = FVector4f(Quat.X, Quat.Y, Quat.Z, Quat.W);
		ScaleOpacityData[PointIndex] = FVector4f(Scale.X, Scale.Y, Scale.Z, Color.A);
		ColorData[PointIndex] = FVector4f(Color.R, Color.G, Color.B, Color.A);

		const int32 SHSrcBase = PointIndex * GS_SH_REST_COUNT;
		const int32 SHDstBase = PointIndex * GS_GPU_SH_FLOAT4S_PER_POINT;
		for (int32 SlotIndex = 0; SlotIndex < GS_GPU_SH_FLOAT4S_PER_POINT; ++SlotIndex)
		{
			const int32 CoeffBase = SHSrcBase + SlotIndex * 4;
			const float C0 = (CoeffBase + 0 < SHSrcBase + GS_SH_REST_COUNT) ? SHCoeffs[CoeffBase + 0] : 0.0f;
			const float C1 = (CoeffBase + 1 < SHSrcBase + GS_SH_REST_COUNT) ? SHCoeffs[CoeffBase + 1] : 0.0f;
			const float C2 = (CoeffBase + 2 < SHSrcBase + GS_SH_REST_COUNT) ? SHCoeffs[CoeffBase + 2] : 0.0f;
			const float C3 = (CoeffBase + 3 < SHSrcBase + GS_SH_REST_COUNT) ? SHCoeffs[CoeffBase + 3] : 0.0f;
			SHRestData[SHDstBase + SlotIndex] = FVector4f(C0, C1, C2, C3);
		}
	}

	ENQUEUE_RENDER_COMMAND(FUpdateGaussianSplattingPointCloudBuffers)(
		[this, PositionDegreeData, RotationData, ScaleOpacityData, ColorData, SHRestData](FRHICommandListImmediate& RHICmdList)
		{
			FScopeLock ScopeLock(&BufferLock);

			auto UploadBuffer = [&RHICmdList](FReadBuffer& Buffer, const TCHAR* DebugName, const TArray<FVector4f>& Data)
			{
				const uint32 NumBytesInBuffer = sizeof(FVector4f) * Data.Num();
				if (NumBytesInBuffer != Buffer.NumBytes)
				{
					if (Buffer.NumBytes > 0)
					{
						Buffer.Release();
					}
					if (NumBytesInBuffer > 0)
					{
						Buffer.Initialize(
							RHICmdList,
							DebugName,
							sizeof(FVector4f),
							Data.Num(),
							EPixelFormat::PF_A32B32G32R32F,
							BUF_Static);
					}
				}

				if (NumBytesInBuffer > 0 && Buffer.NumBytes > 0)
				{
					void* BufferData = RHICmdList.LockBuffer(Buffer.Buffer, 0, NumBytesInBuffer, EResourceLockMode::RLM_WriteOnly);
					FPlatformMemory::Memcpy(BufferData, Data.GetData(), NumBytesInBuffer);
					RHICmdList.UnlockBuffer(Buffer.Buffer);
				}
			};

			UploadBuffer(PositionDegreeBuffer, TEXT("FNiagaraDataInterfaceGaussianSplatting_PositionDegreeBuffer"), PositionDegreeData);
			UploadBuffer(RotationBuffer, TEXT("FNiagaraDataInterfaceGaussianSplatting_RotationBuffer"), RotationData);
			UploadBuffer(ScaleOpacityBuffer, TEXT("FNiagaraDataInterfaceGaussianSplatting_ScaleOpacityBuffer"), ScaleOpacityData);
			UploadBuffer(ColorBuffer, TEXT("FNiagaraDataInterfaceGaussianSplatting_ColorBuffer"), ColorData);
			UploadBuffer(SHRestBuffer, TEXT("FNiagaraDataInterfaceGaussianSplatting_SHRestBuffer"), SHRestData);
		});
}
UNiagaraDataInterfaceGaussianSplattingPointCloud::UNiagaraDataInterfaceGaussianSplattingPointCloud(FObjectInitializer const& ObjectInitializer)
	: Super(ObjectInitializer)

{
	if (!HasAnyFlags(RF_ClassDefaultObject)){
		Proxy = MakeUnique<FNiagaraDataInterfaceProxyGaussianSplattingPointCloud>(this);
	}
}

#if WITH_EDITORONLY_DATA

#if UE_VERSION_NEWER_THAN(5, 4, 0)
void UNiagaraDataInterfaceGaussianSplattingPointCloud::GetFunctionsInternal(TArray<FNiagaraFunctionSignature>& OutFunctions) const
{
	Super::GetFunctionsInternal(OutFunctions);

#else
void UNiagaraDataInterfaceGaussianSplattingPointCloud::GetFunctions(TArray<FNiagaraFunctionSignature>& OutFunctions) 
{
	Super::GetFunctions(OutFunctions);
#endif
	{
		// Original GetPointData - 4 outputs, backwards compatible
		FNiagaraFunctionSignature GetPointDataSignature;
		GetPointDataSignature.Name = GetPointDataFunctionName;
		GetPointDataSignature.Inputs.Add(FNiagaraVariable(GetClass(), TEXT("GaussianSplattingPointCloud")));
		GetPointDataSignature.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index")));
		GetPointDataSignature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Position")));
		GetPointDataSignature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec4Def(), TEXT("Quat")));
		GetPointDataSignature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Scale")));
		GetPointDataSignature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetColorDef(), TEXT("Color")));
		GetPointDataSignature.bMemberFunction = true;
		GetPointDataSignature.bRequiresContext = false;
		OutFunctions.Add(GetPointDataSignature);
	}
	{
		// New GetPointDataSH - includes SH coefficients
		FNiagaraFunctionSignature GetPointDataSHSignature;
		GetPointDataSHSignature.Name = GetPointDataSHFunctionName;
		GetPointDataSHSignature.Inputs.Add(FNiagaraVariable(GetClass(), TEXT("GaussianSplattingPointCloud")));
		GetPointDataSHSignature.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index")));
		GetPointDataSHSignature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Position")));
		GetPointDataSHSignature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec4Def(), TEXT("Quat")));
		GetPointDataSHSignature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Scale")));
		GetPointDataSHSignature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetColorDef(), TEXT("Color")));
		GetPointDataSHSignature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("SHDegree")));
		GetPointDataSHSignature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec4Def(), TEXT("SH0")));
		GetPointDataSHSignature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec4Def(), TEXT("SH1")));
		GetPointDataSHSignature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec4Def(), TEXT("SH2")));
		GetPointDataSHSignature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec4Def(), TEXT("SH3")));
		GetPointDataSHSignature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec4Def(), TEXT("SH4")));
		GetPointDataSHSignature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec4Def(), TEXT("SH5")));
		GetPointDataSHSignature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec4Def(), TEXT("SH6")));
		GetPointDataSHSignature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec4Def(), TEXT("SH7")));
		GetPointDataSHSignature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec4Def(), TEXT("SH8")));
		GetPointDataSHSignature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec4Def(), TEXT("SH9")));
		GetPointDataSHSignature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec4Def(), TEXT("SH10")));
		GetPointDataSHSignature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec4Def(), TEXT("SH11")));
		GetPointDataSHSignature.bMemberFunction = true;
		GetPointDataSHSignature.bRequiresContext = false;
		OutFunctions.Add(GetPointDataSHSignature);
	}
	{
		FNiagaraFunctionSignature GetPointCountSignature;
		GetPointCountSignature.Name = GetPointCountFunctionName;
		GetPointCountSignature.Inputs.Add(FNiagaraVariable(GetClass(), TEXT("GaussianSplattingPointCloud")));
		GetPointCountSignature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("PointCount")));
		GetPointCountSignature.bMemberFunction = true;
		GetPointCountSignature.bRequiresContext = false;
		OutFunctions.Add(GetPointCountSignature);
	}
}

#endif

DEFINE_NDI_DIRECT_FUNC_BINDER(UNiagaraDataInterfaceGaussianSplattingPointCloud, GetPointCount);
DEFINE_NDI_DIRECT_FUNC_BINDER(UNiagaraDataInterfaceGaussianSplattingPointCloud, GetPointData);

void UNiagaraDataInterfaceGaussianSplattingPointCloud::SetPointCloud(UGaussianSplattingPointCloud* InPointCloud)
{
	PointCloud = InPointCloud;
	if (auto DIProxy = GetProxyAs<FNiagaraDataInterfaceProxyGaussianSplattingPointCloud>()) {
		DIProxy->MakeBufferDirty();
	}
}

UGaussianSplattingPointCloud* UNiagaraDataInterfaceGaussianSplattingPointCloud::GetPointCloud() const
{
	return PointCloud;
}

void UNiagaraDataInterfaceGaussianSplattingPointCloud::GetPointCount(FVectorVMExternalFunctionContext& Context)
{
	VectorVM::FExternalFuncRegisterHandler<int32> OutPointCount(Context);
	const int32 Count = PointCloud ? PointCloud->GetPointCount() : 0;

	for (int32 InstanceIdx = 0; InstanceIdx < Context.GetNumInstances(); ++InstanceIdx)
	{
		*OutPointCount.GetDestAndAdvance() = Count;
	}
}
void UNiagaraDataInterfaceGaussianSplattingPointCloud::GetPointData(FVectorVMExternalFunctionContext& Context)
{
	VectorVM::FExternalFuncInputHandler<int32> InIndex(Context);
	VectorVM::FExternalFuncRegisterHandler<float> PosX(Context);
	VectorVM::FExternalFuncRegisterHandler<float> PosY(Context);
	VectorVM::FExternalFuncRegisterHandler<float> PosZ(Context);

	VectorVM::FExternalFuncRegisterHandler<float> QuatX(Context);
	VectorVM::FExternalFuncRegisterHandler<float> QuatY(Context);
	VectorVM::FExternalFuncRegisterHandler<float> QuatZ(Context);
	VectorVM::FExternalFuncRegisterHandler<float> QuatW(Context);

	VectorVM::FExternalFuncRegisterHandler<float> ScaleX(Context);
	VectorVM::FExternalFuncRegisterHandler<float> ScaleY(Context);
	VectorVM::FExternalFuncRegisterHandler<float> ScaleZ(Context);

	VectorVM::FExternalFuncRegisterHandler<float> ColorR(Context);
	VectorVM::FExternalFuncRegisterHandler<float> ColorG(Context);
	VectorVM::FExternalFuncRegisterHandler<float> ColorB(Context);
	VectorVM::FExternalFuncRegisterHandler<float> ColorA(Context);

	const TArray<FVector3f>* PositionsPtr = PointCloud ? &PointCloud->GetPositionsSOA() : nullptr;
	const TArray<FQuat4f>* QuatsPtr = PointCloud ? &PointCloud->GetQuatsSOA() : nullptr;
	const TArray<FVector3f>* ScalesPtr = PointCloud ? &PointCloud->GetScalesSOA() : nullptr;
	const TArray<FLinearColor>* ColorsPtr = PointCloud ? &PointCloud->GetColorsSOA() : nullptr;
	const int32 NumPoints = PositionsPtr ? PositionsPtr->Num() : 0;

	for (int32 InstanceIdx = 0; InstanceIdx < Context.GetNumInstances(); ++InstanceIdx)
	{
		const int32 Index = InIndex.Get();

		FVector3f Position = FVector3f::ZeroVector;
		FQuat4f Quat = FQuat4f::Identity;
		FVector3f Scale = FVector3f(1.0f, 1.0f, 1.0f);
		FLinearColor Color = FLinearColor::Black;

		if (NumPoints > 0 && QuatsPtr && ScalesPtr && ColorsPtr &&
			QuatsPtr->Num() == NumPoints && ScalesPtr->Num() == NumPoints && ColorsPtr->Num() == NumPoints)
		{
			const int32 SafeIndex = FMath::Clamp(Index, 0, NumPoints - 1);
			Position = (*PositionsPtr)[SafeIndex];
			Quat = (*QuatsPtr)[SafeIndex];
			Scale = (*ScalesPtr)[SafeIndex];
			Color = (*ColorsPtr)[SafeIndex];
		}

		*PosX.GetDest() = Position.X;
		*PosY.GetDest() = Position.Y;
		*PosZ.GetDest() = Position.Z;

		*QuatX.GetDest() = Quat.X;
		*QuatY.GetDest() = Quat.Y;
		*QuatZ.GetDest() = Quat.Z;
		*QuatW.GetDest() = Quat.W;

		*ScaleX.GetDest() = Scale.X;
		*ScaleY.GetDest() = Scale.Y;
		*ScaleZ.GetDest() = Scale.Z;

		*ColorR.GetDest() = Color.R;
		*ColorG.GetDest() = Color.G;
		*ColorB.GetDest() = Color.B;
		*ColorA.GetDest() = Color.A;

		InIndex.Advance();

		PosX.Advance();
		PosY.Advance();
		PosZ.Advance();

		QuatX.Advance();
		QuatY.Advance();
		QuatZ.Advance();
		QuatW.Advance();

		ScaleX.Advance();
		ScaleY.Advance();
		ScaleZ.Advance();

		ColorR.Advance();
		ColorG.Advance();
		ColorB.Advance();
		ColorA.Advance();
	}
}
void UNiagaraDataInterfaceGaussianSplattingPointCloud::GetVMExternalFunction(const FVMExternalFunctionBindingInfo& BindingInfo, void* InstanceData, FVMExternalFunction& OutFunc)
{
	if (BindingInfo.Name == GetPointDataFunctionName){
		NDI_FUNC_BINDER(UNiagaraDataInterfaceGaussianSplattingPointCloud, GetPointData)::Bind(this, OutFunc);
	}
	else if (BindingInfo.Name == GetPointDataSHFunctionName){
		// GetPointDataSH uses same CPU path as GetPointData (CPU path doesn't use SH anyway)
		NDI_FUNC_BINDER(UNiagaraDataInterfaceGaussianSplattingPointCloud, GetPointData)::Bind(this, OutFunc);
	}
	else if (BindingInfo.Name == GetPointCountFunctionName){
		NDI_FUNC_BINDER(UNiagaraDataInterfaceGaussianSplattingPointCloud, GetPointCount)::Bind(this, OutFunc);
	}
	else{
		ensureMsgf(false, TEXT("Error! Function defined for this class but not bound."));
	}
}

#if WITH_EDITORONLY_DATA
bool UNiagaraDataInterfaceGaussianSplattingPointCloud::AppendCompileHash(FNiagaraCompileHashVisitor* InVisitor) const
{
	bool bSuccess = Super::AppendCompileHash(InVisitor);
	bSuccess &= InVisitor->UpdateShaderParameters<FShaderParameters>();
	return bSuccess;
}

bool UNiagaraDataInterfaceGaussianSplattingPointCloud::GetFunctionHLSL(const FNiagaraDataInterfaceGPUParamInfo& ParamInfo, const FNiagaraDataInterfaceGeneratedFunction& FunctionInfo, int FunctionInstanceIndex, FString& OutHLSL)
{
	bool ParentRet = Super::GetFunctionHLSL(ParamInfo, FunctionInfo, FunctionInstanceIndex, OutHLSL);
	if (ParentRet)
	{
		return true;
	}
	else if (FunctionInfo.DefinitionName == GetPointDataFunctionName)
	{
		// Original GetPointData - backwards compatible, 4 outputs
		static const TCHAR* FormatBounds = TEXT(R"(
			void {FunctionName}(int In_PointIndex, out float3 Out_Position, out float4 Out_Quat, out float3 Out_Scale, out float4 Out_Color)
			{
				int PointIndex = In_PointIndex < {PointCount} ? In_PointIndex : {PointCount} - 1;
				float4 PositionDegree = {PositionDegreeBuffer}.Load(PointIndex);
				float4 ScaleOpacity = {ScaleOpacityBuffer}.Load(PointIndex);
				Out_Position = PositionDegree.xyz;
				Out_Quat = {RotationBuffer}.Load(PointIndex);
				Out_Scale = ScaleOpacity.xyz;
				Out_Color = {ColorBuffer}.Load(PointIndex);
				Out_Color.w = ScaleOpacity.w;
			}
		)");

		TMap<FString, FStringFormatArg> ArgsBounds = {
			{TEXT("FunctionName"), FStringFormatArg(FunctionInfo.InstanceName)},
			{TEXT("PointCount"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + PointCountName)},
			{TEXT("PositionDegreeBuffer"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + PositionDegreeBufferName)},
			{TEXT("RotationBuffer"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + RotationBufferName)},
			{TEXT("ScaleOpacityBuffer"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + ScaleOpacityBufferName)},
			{TEXT("ColorBuffer"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + ColorBufferName)},
		};
		OutHLSL += FString::Format(FormatBounds, ArgsBounds);
		return true;
	}
	else if (FunctionInfo.DefinitionName == GetPointDataSHFunctionName)
	{
		// New GetPointDataSH - full SH data, 16 outputs
		static const TCHAR* FormatBounds = TEXT(R"(
			void {FunctionName}(int In_PointIndex, out float3 Out_Position, out float4 Out_Quat, out float3 Out_Scale, out float4 Out_Color,
				out int Out_SHDegree,
				out float4 Out_SH0, out float4 Out_SH1, out float4 Out_SH2,
				out float4 Out_SH3, out float4 Out_SH4, out float4 Out_SH5,
				out float4 Out_SH6, out float4 Out_SH7, out float4 Out_SH8,
				out float4 Out_SH9, out float4 Out_SH10, out float4 Out_SH11)
			{
				int PointIndex = In_PointIndex < {PointCount} ? In_PointIndex : {PointCount} - 1;
				float4 PositionDegree = {PositionDegreeBuffer}.Load(PointIndex);
				float4 ScaleOpacity = {ScaleOpacityBuffer}.Load(PointIndex);
				int SHBase = PointIndex * 12;
				Out_Position = PositionDegree.xyz;
				Out_SHDegree = (int)PositionDegree.w;
				Out_Quat = {RotationBuffer}.Load(PointIndex);
				Out_Scale = ScaleOpacity.xyz;
				Out_Color = {ColorBuffer}.Load(PointIndex);
				Out_Color.w = ScaleOpacity.w;
				Out_SH0 = {SHRestBuffer}.Load(SHBase + 0);
				Out_SH1 = {SHRestBuffer}.Load(SHBase + 1);
				Out_SH2 = {SHRestBuffer}.Load(SHBase + 2);
				Out_SH3 = {SHRestBuffer}.Load(SHBase + 3);
				Out_SH4 = {SHRestBuffer}.Load(SHBase + 4);
				Out_SH5 = {SHRestBuffer}.Load(SHBase + 5);
				Out_SH6 = {SHRestBuffer}.Load(SHBase + 6);
				Out_SH7 = {SHRestBuffer}.Load(SHBase + 7);
				Out_SH8 = {SHRestBuffer}.Load(SHBase + 8);
				Out_SH9 = {SHRestBuffer}.Load(SHBase + 9);
				Out_SH10 = {SHRestBuffer}.Load(SHBase + 10);
				Out_SH11 = {SHRestBuffer}.Load(SHBase + 11);
			}
		)");

		TMap<FString, FStringFormatArg> ArgsBounds = {
			{TEXT("FunctionName"), FStringFormatArg(FunctionInfo.InstanceName)},
			{TEXT("PointCount"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + PointCountName)},
			{TEXT("PositionDegreeBuffer"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + PositionDegreeBufferName)},
			{TEXT("RotationBuffer"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + RotationBufferName)},
			{TEXT("ScaleOpacityBuffer"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + ScaleOpacityBufferName)},
			{TEXT("ColorBuffer"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + ColorBufferName)},
			{TEXT("SHRestBuffer"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + SHRestBufferName)},
		};
		OutHLSL += FString::Format(FormatBounds, ArgsBounds);
		return true;
	}
	else if (FunctionInfo.DefinitionName == GetPointCountFunctionName)
	{
		static const TCHAR* FormatBounds = TEXT(R"(
			void {FunctionName}(out int Out_Val)
			{
				Out_Val = {PointCount};
			}
		)");
		TMap<FString, FStringFormatArg> ArgsBounds = {
			{TEXT("FunctionName"), FStringFormatArg(FunctionInfo.InstanceName)},
			{TEXT("PointCount"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + PointCountName)},
		};
		OutHLSL += FString::Format(FormatBounds, ArgsBounds);
		return true;
	}
	else
	{
		return false;
	}
}

void UNiagaraDataInterfaceGaussianSplattingPointCloud::GetParameterDefinitionHLSL(const FNiagaraDataInterfaceGPUParamInfo& ParamInfo, FString& OutHLSL)
{
	Super::GetParameterDefinitionHLSL(ParamInfo, OutHLSL);

	static const TCHAR* FormatDeclarations = TEXT(R"(		
		int {PointCountName};
		Buffer<float4> {PositionDegreeBufferName};
		Buffer<float4> {RotationBufferName};
		Buffer<float4> {ScaleOpacityBufferName};
		Buffer<float4> {ColorBufferName};
		Buffer<float4> {SHRestBufferName};
	)");

	TMap<FString, FStringFormatArg> ArgsDeclarations = {
		{TEXT("PointCountName"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + PointCountName)},
		{TEXT("PositionDegreeBufferName"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + PositionDegreeBufferName)},
		{TEXT("RotationBufferName"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + RotationBufferName)},
		{TEXT("ScaleOpacityBufferName"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + ScaleOpacityBufferName)},
		{TEXT("ColorBufferName"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + ColorBufferName)},
		{TEXT("SHRestBufferName"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + SHRestBufferName)},
	};
	OutHLSL += FString::Format(FormatDeclarations, ArgsDeclarations);
}
#endif

void UNiagaraDataInterfaceGaussianSplattingPointCloud::BuildShaderParameters(FNiagaraShaderParametersBuilder& ShaderParametersBuilder) const
{
	ShaderParametersBuilder.AddNestedStruct<FShaderParameters>();
}

void UNiagaraDataInterfaceGaussianSplattingPointCloud::SetShaderParameters(const FNiagaraDataInterfaceSetShaderParametersContext& Context) const
{
	FNiagaraDataInterfaceProxyGaussianSplattingPointCloud& DIProxy = Context.GetProxy<FNiagaraDataInterfaceProxyGaussianSplattingPointCloud>();
	DIProxy.TryUpdateBuffer();
	UNiagaraDataInterfaceGaussianSplattingPointCloud* Current = DIProxy.Owner;
	FShaderParameters* ShaderParameters = Context.GetParameterNestedStruct<FShaderParameters>();
	ShaderParameters->PointCount = Current->PointCloud ? Current->PointCloud->GetPointCount() : 0;
	ShaderParameters->PositionDegreeBuffer = FNiagaraRenderer::GetSrvOrDefaultFloat4(DIProxy.PositionDegreeBuffer.SRV);
	ShaderParameters->RotationBuffer = FNiagaraRenderer::GetSrvOrDefaultFloat4(DIProxy.RotationBuffer.SRV);
	ShaderParameters->ScaleOpacityBuffer = FNiagaraRenderer::GetSrvOrDefaultFloat4(DIProxy.ScaleOpacityBuffer.SRV);
	ShaderParameters->ColorBuffer = FNiagaraRenderer::GetSrvOrDefaultFloat4(DIProxy.ColorBuffer.SRV);
	ShaderParameters->SHRestBuffer = FNiagaraRenderer::GetSrvOrDefaultFloat4(DIProxy.SHRestBuffer.SRV);
}
bool UNiagaraDataInterfaceGaussianSplattingPointCloud::Equals(const UNiagaraDataInterface* Other) const
{
	bool bIsEqual = Super::Equals(Other);
	const UNiagaraDataInterfaceGaussianSplattingPointCloud* OtherPointCloud = CastChecked<const UNiagaraDataInterfaceGaussianSplattingPointCloud>(Other);
	return OtherPointCloud->PointCloud == PointCloud;
}

#if WITH_EDITOR
void UNiagaraDataInterfaceGaussianSplattingPointCloud::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	static const FName PointCloudFName = GET_MEMBER_NAME_CHECKED(UNiagaraDataInterfaceGaussianSplattingPointCloud, PointCloud);

	if (!HasAnyFlags(RF_ClassDefaultObject)){
		if (PropertyChangedEvent.GetMemberPropertyName() == PointCloudFName) {
			GetProxyAs<FNiagaraDataInterfaceProxyGaussianSplattingPointCloud>()->MakeBufferDirty();
		}
	}
}
#endif //WITH_EDITOR

void UNiagaraDataInterfaceGaussianSplattingPointCloud::PostInitProperties()
{
	Super::PostInitProperties();

	if (HasAnyFlags(RF_ClassDefaultObject)){
		ENiagaraTypeRegistryFlags Flags = ENiagaraTypeRegistryFlags::AllowAnyVariable | ENiagaraTypeRegistryFlags::AllowParameter;
		FNiagaraTypeRegistry::Register(FNiagaraTypeDefinition(GetClass()), Flags);
	}
	else {
		GetProxyAs<FNiagaraDataInterfaceProxyGaussianSplattingPointCloud>()->MakeBufferDirty();
	}
}

void UNiagaraDataInterfaceGaussianSplattingPointCloud::PostLoad()
{
	Super::PostLoad();
}

bool UNiagaraDataInterfaceGaussianSplattingPointCloud::CopyToInternal(UNiagaraDataInterface* Destination) const
{
	if (!Super::CopyToInternal(Destination)){
		return false;
	}

	UNiagaraDataInterfaceGaussianSplattingPointCloud* CastedDestination = Cast<UNiagaraDataInterfaceGaussianSplattingPointCloud>(Destination);
	if (CastedDestination){
		CastedDestination->PointCloud = PointCloud;
	}
	return true;
}


// Global VM function names, also used by the shaders code generation methods.
const FName UNiagaraDataInterfaceGaussianSplattingPointCloud::GetPointDataFunctionName("GetPointData");
const FName UNiagaraDataInterfaceGaussianSplattingPointCloud::GetPointDataSHFunctionName("GetPointDataSH");
const FName UNiagaraDataInterfaceGaussianSplattingPointCloud::GetPointCountFunctionName("GetPointCount");

// Global variable prefixes, used in HLSL parameter declarations.
const FString UNiagaraDataInterfaceGaussianSplattingPointCloud::PointCountName(TEXT("_PointCount"));
const FString UNiagaraDataInterfaceGaussianSplattingPointCloud::PositionDegreeBufferName(TEXT("_PositionDegreeBuffer"));
const FString UNiagaraDataInterfaceGaussianSplattingPointCloud::RotationBufferName(TEXT("_RotationBuffer"));
const FString UNiagaraDataInterfaceGaussianSplattingPointCloud::ScaleOpacityBufferName(TEXT("_ScaleOpacityBuffer"));
const FString UNiagaraDataInterfaceGaussianSplattingPointCloud::ColorBufferName(TEXT("_ColorBuffer"));
const FString UNiagaraDataInterfaceGaussianSplattingPointCloud::SHRestBufferName(TEXT("_SHRestBuffer"));
#undef LOCTEXT_NAMESPACE



