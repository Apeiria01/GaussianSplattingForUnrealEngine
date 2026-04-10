#pragma once

#include "CoreMinimal.h"
#include "NiagaraSpriteRendererProperties.h"
#include "GaussianNiagaraSpriteRendererProperties.generated.h"

UCLASS(editinlinenew, meta = (DisplayName = "Gaussian Sprite Renderer", SupportsStateless))
class GAUSSIANSPLATTINGRUNTIME_API UGaussianNiagaraSpriteRendererProperties : public UNiagaraSpriteRendererProperties
{
	GENERATED_BODY()

public:
	virtual FNiagaraRenderer* CreateEmitterRenderer(
		ERHIFeatureLevel::Type FeatureLevel,
		const FNiagaraEmitterInstance* Emitter,
		const FNiagaraSystemInstanceController& InController) override;
};
