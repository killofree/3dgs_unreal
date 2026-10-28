#pragma once

#include "IDetailCustomization.h"

class FGaussianSplatDetails final : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;
};
