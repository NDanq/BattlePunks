#ifndef CORECLIENT_UISHADERS_H
#define CORECLIENT_UISHADERS_H

#include "Public/GDI/GDIShader.h"
#include <string>

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

class Screen_VS : public GDIShader
{
public:
    Screen_VS(std::string InName) : GDIShader(InName){}
    virtual void InitUniformBindings() override
    {

    }
};

class Screen_PS : public GDIShader
{
public:
    Screen_PS(std::string InName) : GDIShader(InName){  }
    virtual void InitUniformBindings() override
    {
        RegisterTexUniform("ScreenTexture", nullptr, 0);
    }

    void SetScreenBlitTexture(GDITexture2D* InTexture)
    {
        SetUniform("ScreenTexture", InTexture);
    }
};

class LayerBlit_PS : public GDIShader
{
public:
    LayerBlit_PS(std::string InName) : GDIShader(InName){  }
    virtual void InitUniformBindings() override
    {
        RegisterTexUniform("ScreenTexture", nullptr, 0);
        RegisterUniform("Opacity", 1.f);
    }

    void SetParams(GDITexture2D* InTexture, const uint& InAlpha)
    {
        SetUniform("ScreenTexture", InTexture);
        SetUniform("Opacity", (float(InAlpha) / 100.f));
    }
};

class Checkerboard_PS : public GDIShader
{
public:
    Checkerboard_PS(std::string InName) : GDIShader(InName){  }
    virtual void InitUniformBindings() override
    {

    }
};

class Cursor_PS : public GDIShader
{
public:
    Cursor_PS(std::string InName) : GDIShader(InName)
    {
    }

    void InitUniformBindings() override
    {
        RegisterTexUniform("Stencil", nullptr, 0);
    }

    inline void Set(GDITexture2D* InTexture)
    {
        SetUniform("Stencil", InTexture);
    }
};

#endif //CORECLIENT_UISHADERS_H
