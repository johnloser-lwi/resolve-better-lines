#pragma once

#include "ofxsImageEffect.h"

#define kPluginName        "Better Line"
#define kPluginNameGenerator "Better Line Generator"
#define kPluginGrouping    "BetterLine"
#define kPluginDescription "Editable, styleable, animatable lines and curves."
#define kPluginIdentifier  "com.resolvebetterline.BetterLine"
// A DISTINCT id, not a variant of the one above: the host keys saved effects on
// it, so the two must never collide.
#define kPluginIdentifierGenerator "com.resolvebetterline.BetterLineGenerator"
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

/** @brief The generator, as a separate plugin.
 *
 * A separate CLASS, not a second instance of the one above. PluginFactoryHelper
 * derives from FactoryMainEntryHelper<FACTORY>, which holds a static _uid per
 * template instantiation and asserts its constructor runs only once; two
 * instances of one class would share a uid and route both plugins through a
 * single main entry.
 *
 * Both create the same BetterLinePlugin -- the difference is entirely in what is
 * declared, not in what runs.
 */
class BetterLineGeneratorFactory : public OFX::PluginFactoryHelper<BetterLineGeneratorFactory>
{
public:
    BetterLineGeneratorFactory();

    void describe(OFX::ImageEffectDescriptor& p_Desc) override;
    void describeInContext(OFX::ImageEffectDescriptor& p_Desc, OFX::ContextEnum p_Context) override;
    OFX::ImageEffect* createInstance(OfxImageEffectHandle p_Handle, OFX::ContextEnum p_Context) override;
};
