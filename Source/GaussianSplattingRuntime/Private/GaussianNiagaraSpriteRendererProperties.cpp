#include "GaussianNiagaraSpriteRendererProperties.h"

#include "GaussianNiagaraRendererSprites.h"

FNiagaraRenderer* UGaussianNiagaraSpriteRendererProperties::CreateEmitterRenderer(
	ERHIFeatureLevel::Type FeatureLevel,
	const FNiagaraEmitterInstance* Emitter,
	const FNiagaraSystemInstanceController& InController)
{
	FNiagaraRenderer* NewRenderer = new FGaussianNiagaraRendererSprites(FeatureLevel, this, Emitter);
	NewRenderer->Initialize(this, Emitter, InController);
	return NewRenderer;
}
