#ifndef ABSTRACT_MULTISCALE_BODY_UI_HPP
#define ABSTRACT_MULTISCALE_BODY_UI_HPP
#include <string>
#include <cstdint>
START_NAMESPACE_DISTRHO
class AbstractMultiScaleBodyUI {
public:
    virtual ~AbstractMultiScaleBodyUI() {}
    virtual float getParamValue(uint32_t index) const = 0;
    virtual void setParamValue(uint32_t index, float value) = 0;
    virtual void editParameter(uint32_t index, bool start) = 0;
    virtual void syncParamWidget(uint32_t index, float value) = 0;
    virtual std::string parameterName(uint32_t index) const = 0;
};
END_NAMESPACE_DISTRHO
#endif
