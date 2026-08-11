#pragma once

#include "ofxsImageEffect.h"

#define kPluginName        "Better Line"
#define kPluginNameGenerator "Better Line Generator"
#define kPluginGrouping    "BetterLine"
#define kPluginDescription "Editable, styleable, animatable lines and curves."
#define kPluginIdentifier  "com.resolvebetterline.BetterLine"
#define kPluginVersionMajor 0
#define kPluginVersionMinor 1

class BetterLinePluginFactory : public OFX::PluginFactoryHelper<BetterLinePluginFactory>
{
public:
    BetterLinePluginFactory();

    void describe(OFX::ImageEffectDescriptor& p_Desc) override;
    void describeInContext(OFX::ImageEffectDescriptor& p_Desc, OFX::ContextEnum p_Context) override;
    OFX::ImageEffect* createInstance(OfxImageEffectHandle p_Handle, OFX::ContextEnum p_Context) override;
};
