/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX, based on Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "SolvfxOcioViewer.h"

#include "Global/GLIncludes.h" // must be included before Qt OpenGL headers

#include <cstdlib>
#include <stdexcept>

#include <QDebug>
#include <QOpenGLShaderProgram>
#include <QString>

#ifdef SOLVFX_HAS_OCIO
#include <OpenColorIO/OpenColorIO.h>
namespace OCIO = OCIO_NAMESPACE;
#endif

NATRON_NAMESPACE_ENTER

#ifdef SOLVFX_HAS_OCIO
namespace {
struct LutTexture
{
    GLuint id = 0;
    GLenum target = GL_TEXTURE_2D;
    std::string samplerName;
};

struct UniformInfo
{
    std::string name;
    OCIO::GpuShaderDesc::UniformData data;
};
}
#endif

struct SolvfxOcioViewerShader::Imp
{
    std::string display, view;
    bool dirty = true;
    bool failed = false;
    std::unique_ptr<QOpenGLShaderProgram> program;
#ifdef SOLVFX_HAS_OCIO
    std::vector<LutTexture> textures;
    std::vector<UniformInfo> uniforms;
#endif

    void clearGL()
    {
#ifdef SOLVFX_HAS_OCIO
        for (std::size_t i = 0; i < textures.size(); ++i) {
            if (textures[i].id) {
                glDeleteTextures(1, &textures[i].id);
            }
        }
        textures.clear();
        uniforms.clear();
#endif
        program.reset();
    }

    bool build();
};

static const char* kSolvfxOcioVertex =
    "#version 120\n"
    "void main()\n"
    "{\n"
    "    gl_TexCoord[0] = gl_MultiTexCoord0;\n"
    "    gl_Position = ftransform();\n"
    "}\n";

bool
SolvfxOcioViewerShader::Imp::build()
{
    clearGL();
#ifndef SOLVFX_HAS_OCIO

    return false;
#else
    try {
        OCIO::ConstConfigRcPtr config = OCIO::Config::CreateFromEnv();
        OCIO::DisplayViewTransformRcPtr dv = OCIO::DisplayViewTransform::Create();
        dv->setSrc(OCIO::ROLE_SCENE_LINEAR);
        dv->setDisplay( display.c_str() );
        dv->setView( view.c_str() );
        OCIO::ConstProcessorRcPtr processor = config->getProcessor(dv);

        OCIO::GpuShaderDescRcPtr desc = OCIO::GpuShaderDesc::CreateShaderDesc();
        desc->setLanguage(OCIO::GPU_LANGUAGE_GLSL_1_2);
        desc->setFunctionName("OCIODisplay");
        desc->setResourcePrefix("ocio_");
        processor->getDefaultGPUProcessor()->extractGpuShaderInfo(desc);

        // fragment: gain/offset (linear) -> OCIO view -> gamma -> saturation
        std::string frag = "#version 120\n";
        frag += desc->getShaderText();
        frag +=
            "\n"
            "uniform sampler2D Tex;\n"
            "uniform float gain;\n"
            "uniform float offset;\n"
            "uniform float gamma;\n"
            "uniform float saturation;\n"
            "void main()\n"
            "{\n"
            "    vec4 c = texture2D(Tex, gl_TexCoord[0].st);\n"
            "    c.rgb = c.rgb * gain + offset;\n"
            "    c = OCIODisplay(c);\n"
            "    if (gamma <= 0.) {\n"
            "        c.rgb = vec3(c.r >= 1. ? 1. : 0., c.g >= 1. ? 1. : 0., c.b >= 1. ? 1. : 0.);\n"
            "    } else if (gamma != 1.) {\n"
            "        c.rgb = pow(max(c.rgb, vec3(0.)), vec3(1. / gamma));\n"
            "    }\n"
            "    if (saturation != 1.) {\n"
            "        float luma = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722));\n"
            "        c.rgb = mix(vec3(luma), c.rgb, saturation);\n"
            "    }\n"
            "    gl_FragColor = c;\n"
            "}\n";

        program.reset( new QOpenGLShaderProgram() );
        if ( !program->addShaderFromSourceCode(QOpenGLShader::Vertex, kSolvfxOcioVertex) ||
             !program->addShaderFromSourceCode( QOpenGLShader::Fragment, QString::fromUtf8( frag.c_str() ) ) ||
             !program->link() ) {
            qDebug() << "SOLVFX OCIO viewer shader:" << program->log();
            program.reset();

            return false;
        }

        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        // 3D LUTs
        for (unsigned i = 0; i < desc->getNum3DTextures(); ++i) {
            const char* texName = 0;
            const char* samplerName = 0;
            unsigned edgelen = 0;
            OCIO::Interpolation interp = OCIO::INTERP_LINEAR;
            desc->get3DTexture(i, texName, samplerName, edgelen, interp);
            const float* values = 0;
            desc->get3DTextureValues(i, values);
            LutTexture t;
            t.target = GL_TEXTURE_3D;
            t.samplerName = samplerName ? samplerName : "";
            glGenTextures(1, &t.id);
            glBindTexture(GL_TEXTURE_3D, t.id);
            GLint filter = (interp == OCIO::INTERP_NEAREST) ? GL_NEAREST : GL_LINEAR;
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, filter);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, filter);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
            glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB32F_ARB, edgelen, edgelen, edgelen, 0, GL_RGB, GL_FLOAT, values);
            glBindTexture(GL_TEXTURE_3D, 0);
            textures.push_back(t);
        }

        // 1D / 2D LUTs
        for (unsigned i = 0; i < desc->getNumTextures(); ++i) {
            const char* texName = 0;
            const char* samplerName = 0;
            unsigned width = 0, height = 0;
            OCIO::GpuShaderDesc::TextureType channel = OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL;
            OCIO::Interpolation interp = OCIO::INTERP_LINEAR;
            bool is1D = false;
#if OCIO_VERSION_HEX >= 0x02030000
            OCIO::GpuShaderDesc::TextureDimensions dims = OCIO::GpuShaderDesc::TEXTURE_2D;
            desc->getTexture(i, texName, samplerName, width, height, channel, dims, interp);
            is1D = (dims == OCIO::GpuShaderDesc::TEXTURE_1D);
#else
            desc->getTexture(i, texName, samplerName, width, height, channel, interp);
            is1D = (height <= 1);
#endif
            const float* values = 0;
            desc->getTextureValues(i, values);
            GLenum format = (channel == OCIO::GpuShaderDesc::TEXTURE_RED_CHANNEL) ? GL_RED : GL_RGB;
            LutTexture t;
            t.target = is1D ? GL_TEXTURE_1D : GL_TEXTURE_2D;
            t.samplerName = samplerName ? samplerName : "";
            glGenTextures(1, &t.id);
            glBindTexture(t.target, t.id);
            GLint filter = (interp == OCIO::INTERP_NEAREST) ? GL_NEAREST : GL_LINEAR;
            glTexParameteri(t.target, GL_TEXTURE_MIN_FILTER, filter);
            glTexParameteri(t.target, GL_TEXTURE_MAG_FILTER, filter);
            glTexParameteri(t.target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            if (is1D) {
                glTexImage1D(GL_TEXTURE_1D, 0, GL_RGB32F_ARB, width, 0, format, GL_FLOAT, values);
            } else {
                glTexParameteri(t.target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F_ARB, width, height, 0, format, GL_FLOAT, values);
            }
            glBindTexture(t.target, 0);
            textures.push_back(t);
        }

        // dynamic uniforms (exposure/contrast etc. in some views)
        for (unsigned i = 0; i < desc->getNumUniforms(); ++i) {
            UniformInfo u;
            const char* name = desc->getUniform(i, u.data);
            u.name = name ? name : "";
            uniforms.push_back(u);
        }

        return true;
    } catch (const std::exception& e) {
        qDebug() << "SOLVFX OCIO viewer:" << e.what();
        clearGL();

        return false;
    }
#endif // SOLVFX_HAS_OCIO
} // build

bool
SolvfxOcioViewerShader::listDisplayViews(std::vector<DisplayView>* views,
                                         int* defaultIndex)
{
    views->clear();
    *defaultIndex = -1;
#ifdef SOLVFX_HAS_OCIO
    const char* env = std::getenv("OCIO");
    if ( !env || !env[0] ) {
        return false;
    }
    try {
        OCIO::ConstConfigRcPtr config = OCIO::Config::CreateFromEnv();
        const std::string defDisplay = config->getDefaultDisplay();
        const std::string defView = config->getDefaultView( defDisplay.c_str() );
        for (int d = 0; d < config->getNumDisplays(); ++d) {
            const char* display = config->getDisplay(d);
            for (int v = 0; v < config->getNumViews(display); ++v) {
                DisplayView dv;
                dv.display = display;
                dv.view = config->getView(display, v);
                dv.label = dv.view + " (" + dv.display + ")";
                if ( (dv.display == defDisplay) && (dv.view == defView) ) {
                    *defaultIndex = (int)views->size();
                }
                views->push_back(dv);
            }
        }
        if ( views->empty() ) {
            return false;
        }
        if (*defaultIndex < 0) {
            *defaultIndex = 0;
        }

        return true;
    } catch (const std::exception& e) {
        qDebug() << "SOLVFX OCIO viewer:" << e.what();
        views->clear();

        return false;
    }
#else

    return false;
#endif
}

SolvfxOcioViewerShader::SolvfxOcioViewerShader()
    : _imp(new Imp)
{
}

SolvfxOcioViewerShader::~SolvfxOcioViewerShader()
{
}

void
SolvfxOcioViewerShader::setDisplayView(const std::string& display,
                                       const std::string& view)
{
    if ( (display == _imp->display) && (view == _imp->view) ) {
        return;
    }
    _imp->display = display;
    _imp->view = view;
    _imp->dirty = true;
    _imp->failed = false;
}

bool
SolvfxOcioViewerShader::isActive() const
{
#ifdef SOLVFX_HAS_OCIO

    return !_imp->display.empty() && !_imp->failed;
#else

    return false;
#endif
}

bool
SolvfxOcioViewerShader::bind(float gain,
                             float offset,
                             float gamma,
                             float saturation)
{
#ifndef SOLVFX_HAS_OCIO
    (void)gain; (void)offset; (void)gamma; (void)saturation;

    return false;
#else
    if ( !isActive() ) {
        return false;
    }
    if (_imp->dirty) {
        _imp->dirty = false;
        if ( !_imp->build() ) {
            _imp->failed = true;

            return false;
        }
    }
    if ( !_imp->program || !_imp->program->bind() ) {
        return false;
    }
    _imp->program->setUniformValue("Tex", 0);
    _imp->program->setUniformValue("gain", gain);
    _imp->program->setUniformValue("offset", offset);
    _imp->program->setUniformValue("gamma", gamma);
    _imp->program->setUniformValue("saturation", saturation);
    for (std::size_t i = 0; i < _imp->textures.size(); ++i) {
        glActiveTexture(GL_TEXTURE1 + (GLenum)i);
        glBindTexture(_imp->textures[i].target, _imp->textures[i].id);
        _imp->program->setUniformValue( _imp->textures[i].samplerName.c_str(), (GLint)(1 + i) );
    }
    glActiveTexture(GL_TEXTURE0);
    for (std::size_t i = 0; i < _imp->uniforms.size(); ++i) {
        const UniformInfo& u = _imp->uniforms[i];
        switch (u.data.m_type) {
        case OCIO::UNIFORM_DOUBLE:
            if (u.data.m_getDouble) {
                _imp->program->setUniformValue( u.name.c_str(), (GLfloat)u.data.m_getDouble() );
            }
            break;
        case OCIO::UNIFORM_BOOL:
            if (u.data.m_getBool) {
                _imp->program->setUniformValue( u.name.c_str(), (GLint)(u.data.m_getBool() ? 1 : 0) );
            }
            break;
        case OCIO::UNIFORM_FLOAT3:
            if (u.data.m_getFloat3) {
                const OCIO::Float3& f = u.data.m_getFloat3();
                _imp->program->setUniformValue( u.name.c_str(), f[0], f[1], f[2] );
            }
            break;
        default:
            break;
        }
    }

    return true;
#endif // SOLVFX_HAS_OCIO
} // bind

void
SolvfxOcioViewerShader::release()
{
#ifdef SOLVFX_HAS_OCIO
    for (std::size_t i = 0; i < _imp->textures.size(); ++i) {
        glActiveTexture(GL_TEXTURE1 + (GLenum)i);
        glBindTexture(_imp->textures[i].target, 0);
    }
    glActiveTexture(GL_TEXTURE0);
#endif
    if (_imp->program) {
        _imp->program->release();
    }
}

void
SolvfxOcioViewerShader::destroyGL()
{
    _imp->clearGL();
    _imp->dirty = true;
}

NATRON_NAMESPACE_EXIT
