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
	if (Owner == nullptr || Owner->PointCloud == nullptr) {
		return;
	}
	const TArray<FGaussianSplattingPoint>& Points = Owner->PointCloud->GetPoints();

	// GPU buffer layout: GS_GPU_FLOAT4S_PER_POINT (16) float4 per point
	//  [0]  Position.xyz, SHDegree (as float)
	//  [1]  Quat.xyzw
	//  [2]  Scale.xyz, pad
	//  [3]  Color(DC raw).rgb, Alpha
	//  [4]  SHCoeffs[ 0.. 3]
	//  [5]  SHCoeffs[ 4.. 7]
	//  [6]  SHCoeffs[ 8..11]
	//  [7]  SHCoeffs[12..15]
	//  [8]  SHCoeffs[16..19]
	//  [9]  SHCoeffs[20..23]
	//  [10] SHCoeffs[24..27]
	//  [11] SHCoeffs[28..31]
	//  [12] SHCoeffs[32..35]
	//  [13] SHCoeffs[36..39]
	//  [14] SHCoeffs[40..43]
	//  [15] SHCoeffs[44], pad, pad, pad
	const int32 Float4sPerPoint = GS_GPU_FLOAT4S_PER_POINT;
	TArray<FVector4f> PointData;
	PointData.SetNum(Points.Num() * Float4sPerPoint);
	for (int i = 0; i < Points.Num(); i++) {
		const FGaussianSplattingPoint& Point = Points[i];
		int32 Base = i * Float4sPerPoint;
		PointData[Base + 0] = FVector4f(Point.Position.X, Point.Position.Y, Point.Position.Z, (float)Point.SHDegree);
		PointData[Base + 1] = FVector4f(Point.Quat.X, Point.Quat.Y, Point.Quat.Z, Point.Quat.W);
		PointData[Base + 2] = FVector4f(Point.Scale.X, Point.Scale.Y, Point.Scale.Z, 0.0f);
		PointData[Base + 3] = FVector4f(Point.Color.R, Point.Color.G, Point.Color.B, Point.Color.A);

		// Pack 45 SH coefficients into 12 float4 slots (indices 4..15)
		
		for (int32 j = 0; j < 12; j++)
		{
			float v0 = (j * 4 + 0 < GS_SH_REST_COUNT) ? Point.SHCoeffs[j * 4 + 0] : 0.0f;
			float v1 = (j * 4 + 1 < GS_SH_REST_COUNT) ? Point.SHCoeffs[j * 4 + 1] : 0.0f;
			float v2 = (j * 4 + 2 < GS_SH_REST_COUNT) ? Point.SHCoeffs[j * 4 + 2] : 0.0f;
			float v3 = (j * 4 + 3 < GS_SH_REST_COUNT) ? Point.SHCoeffs[j * 4 + 3] : 0.0f;
			PointData[Base + 4 + j] = FVector4f(v0, v1, v2, v3);
		}
	}

	ENQUEUE_RENDER_COMMAND(FUpdateSpectrumBuffer)(
		[this, PointData](FRHICommandListImmediate& RHICmdList)
		{
			const int32 NumBytesInBuffer = sizeof(FVector4f) * PointData.Num();

			if (NumBytesInBuffer != GaussianPointDataBuffer.NumBytes){
				if (GaussianPointDataBuffer.NumBytes > 0)
					GaussianPointDataBuffer.Release();
				if (NumBytesInBuffer > 0)
					GaussianPointDataBuffer.Initialize(RHICmdList, TEXT("FNiagaraDataInterfaceProxySpectrum_PositionBuffer"), sizeof(FVector4f), PointData.Num(), EPixelFormat::PF_A32B32G32R32F, BUF_Static);
			}

			if (GaussianPointDataBuffer.NumBytes > 0){
				float* BufferData = static_cast<float*>(RHICmdList.LockBuffer(GaussianPointDataBuffer.Buffer, 0, NumBytesInBuffer, EResourceLockMode::RLM_WriteOnly));
				FScopeLock ScopeLock(&BufferLock);
				FPlatformMemory::Memcpy(BufferData, PointData.GetData(), NumBytesInBuffer);
				RHICmdList.UnlockBuffer(GaussianPointDataBuffer.Buffer);
			}
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

	for (int32 InstanceIdx = 0; InstanceIdx < Context.GetNumInstances(); ++InstanceIdx)
	{
		*OutPointCount.GetDestAndAdvance() = PointCloud->GetPoints().Num();
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

	auto Points = PointCloud->GetPoints();
	for (int32 InstanceIdx = 0; InstanceIdx < Context.GetNumInstances(); ++InstanceIdx){
		int32 Index = InIndex.Get();
		FGaussianSplattingPoint& Point = Points[Index];
		*PosX.GetDest() = Point.Position.X;
		*PosX.GetDest() = Point.Position.X;
		*PosX.GetDest() = Point.Position.X;

		*QuatX.GetDest() = Point.Quat.X;
		*QuatY.GetDest() = Point.Quat.Y;
		*QuatZ.GetDest() = Point.Quat.Z;
		*QuatZ.GetDest() = Point.Quat.Z;

		*ScaleX.GetDest() = Point.Scale.X;
		*ScaleY.GetDest() = Point.Scale.Y;
		*ScaleZ.GetDest() = Point.Scale.Z;

		*ColorR.GetDest() = Point.Color.R;
		*ColorG.GetDest() = Point.Color.G;
		*ColorB.GetDest() = Point.Color.B;
		*ColorA.GetDest() = Point.Color.A;

		InIndex.Advance();

		PosX.Advance();
		PosY.Advance();
		PosZ.Advance();

		QuatX.Advance();
		QuatY.Advance();
		QuatZ.Advance();
		QuatZ.Advance();

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
				int Base = PointIndex * 16;
				Out_Position = {PointDataBuffer}.Load(Base + 0).xyz;
				Out_Quat     = {PointDataBuffer}.Load(Base + 1);
				Out_Scale    = {PointDataBuffer}.Load(Base + 2).xyz;
				Out_Color    = {PointDataBuffer}.Load(Base + 3);
			}
		)");

		TMap<FString, FStringFormatArg> ArgsBounds = {
			{TEXT("FunctionName"), FStringFormatArg(FunctionInfo.InstanceName)},
			{TEXT("PointCount"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + PointCountName)},
			{TEXT("PointDataBuffer"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + PointDataBufferName)},
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
				int Base = PointIndex * 16;
				float4 Slot0 = {PointDataBuffer}.Load(Base + 0);
				Out_Position  = Slot0.xyz;
				Out_SHDegree  = (int)Slot0.w;
				Out_Quat      = {PointDataBuffer}.Load(Base + 1);
				Out_Scale     = {PointDataBuffer}.Load(Base + 2).xyz;
				Out_Color     = {PointDataBuffer}.Load(Base + 3);
				Out_SH0       = {PointDataBuffer}.Load(Base + 4);
				Out_SH1       = {PointDataBuffer}.Load(Base + 5);
				Out_SH2       = {PointDataBuffer}.Load(Base + 6);
				Out_SH3       = {PointDataBuffer}.Load(Base + 7);
				Out_SH4       = {PointDataBuffer}.Load(Base + 8);
				Out_SH5       = {PointDataBuffer}.Load(Base + 9);
				Out_SH6       = {PointDataBuffer}.Load(Base + 10);
				Out_SH7       = {PointDataBuffer}.Load(Base + 11);
				Out_SH8       = {PointDataBuffer}.Load(Base + 12);
				Out_SH9       = {PointDataBuffer}.Load(Base + 13);
				Out_SH10      = {PointDataBuffer}.Load(Base + 14);
				Out_SH11      = {PointDataBuffer}.Load(Base + 15);
			}
		)");

		TMap<FString, FStringFormatArg> ArgsBounds = {
			{TEXT("FunctionName"), FStringFormatArg(FunctionInfo.InstanceName)},
			{TEXT("PointCount"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + PointCountName)},
			{TEXT("PointDataBuffer"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + PointDataBufferName)},
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
		Buffer<float4> {PointDataBufferName};
	)");

	TMap<FString, FStringFormatArg> ArgsDeclarations = {
		{TEXT("PointCountName"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + PointCountName)},
		{TEXT("PointDataBufferName"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + PointDataBufferName)},
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
	ShaderParameters->PointCount = Current->PointCloud ? Current->PointCloud->GetPoints().Num() : 0;
	ShaderParameters->PointDataBuffer = FNiagaraRenderer::GetSrvOrDefaultFloat4(DIProxy.GaussianPointDataBuffer.SRV);
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
const FString UNiagaraDataInterfaceGaussianSplattingPointCloud::PointDataBufferName(TEXT("_PointDataBuffer"));

#undef LOCTEXT_NAMESPACE
