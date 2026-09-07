#ifndef CORECLIENT_BRUSHSHADERS_H
#define CORECLIENT_BRUSHSHADERS_H

#include "Public/GDI/GDIShader.h"
#include <string>

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

class UberBrush_PS : public GDIShader {
public:
    UberBrush_PS(std::string InName) : GDIShader(InName) {

    }

    void InitUniformBindings() override {
        RegisterUniform("ColorOpacity", Vector4(0, 0, 0, 1));
        RegisterTexUniform("Stencil", nullptr, 0);
    }

    void Set(const Vector4 &InColor, GDITexture2D *InTex)
    {
        SetUniform("ColorOpacity", InColor);
        SetUniform("Stencil", InTex, 0);
    }
};

#endif //CORECLIENT_BRUSHSHADERS_H
