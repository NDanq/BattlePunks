#ifndef CORECLIENT_TESTBEDSHADERS_H
#define CORECLIENT_TESTBEDSHADERS_H

#include "Public/GDI/GDIShader.h"
#include <string>

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

class Basic_VS : public GDIShader
{
public:
    Basic_VS(std::string InName) : GDIShader(InName)
    {

    }

    void InitUniformBindings() override
    {

    }
};

class Basic_Instanced_VS : public GDIShader
{
public:
    Basic_Instanced_VS(std::string InName) : GDIShader(InName)
    {

    }

    void InitUniformBindings() override
    {

    }
};

class Basic_PS : public GDIShader
{
public:
    Basic_PS(std::string InName) : GDIShader(InName)
    {

    }

    void InitUniformBindings() override
    {
        RegisterUniform("TestColor", Vector3(1,0,1));
    }
};

/* Mesh.vsh: an indexed mesh's vertices, normal at location 8 (GDIMesh.h). */
class Mesh_VS : public GDIShader
{
public:
    Mesh_VS(std::string InName) : GDIShader(InName)
    {

    }

    void InitUniformBindings() override
    {

    }
};

/* Mesh.fsh: an unmaterialed mesh -- its Tint, lit by one fixed light. */
class Mesh_PS : public GDIShader
{
public:
    Mesh_PS(std::string InName) : GDIShader(InName)
    {

    }

    void InitUniformBindings() override
    {
        RegisterUniform("Tint", Vector3(1,1,1));
    }
};


#endif //CORECLIENT_TESTBEDSHADERS_H
