/*
 * Copyright 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <utils/String8.h>

#include "ProgramCache.h"
#include "Program.h"
#include "Description.h"
#include <utils/Trace.h>

namespace android {
// -----------------------------------------------------------------------------------------------


/*
 * A simple formatter class to automatically add the endl and
 * manage the indentation.
 */

class Formatter;
static Formatter& indent(Formatter& f);
static Formatter& dedent(Formatter& f);

class Formatter {
    String8 mString;
    int mIndent;
    typedef Formatter& (*FormaterManipFunc)(Formatter&);
    friend Formatter& indent(Formatter& f);
    friend Formatter& dedent(Formatter& f);
public:
    Formatter() : mIndent(0) {}

    String8 getString() const {
        return mString;
    }

    friend Formatter& operator << (Formatter& out, const char* in) {
        for (int i=0 ; i<out.mIndent ; i++) {
            out.mString.append("    ");
        }
        out.mString.append(in);
        out.mString.append("\n");
        return out;
    }
    friend inline Formatter& operator << (Formatter& out, const String8& in) {
        return operator << (out, in.string());
    }
    friend inline Formatter& operator<<(Formatter& to, FormaterManipFunc func) {
        return (*func)(to);
    }
};
Formatter& indent(Formatter& f) {
    f.mIndent++;
    return f;
}
Formatter& dedent(Formatter& f) {
    f.mIndent--;
    return f;
}

// -----------------------------------------------------------------------------------------------

ANDROID_SINGLETON_STATIC_INSTANCE(ProgramCache)

ProgramCache::ProgramCache() {
    // Until surfaceflinger has a dependable blob cache on the filesystem,
    // generate shaders on initialization so as to avoid jank.
    primeCache();
}

ProgramCache::~ProgramCache() {
}

void ProgramCache::primeCache() {

    uint32_t shaderCount = 0;
    uint32_t keyMask = Key::BLEND_MASK | Key::OPACITY_MASK |
                       Key::PLANE_ALPHA_MASK | Key::TEXTURE_MASK;
    // Prime the cache for all combinations of the above masks,
    // leaving off the experimental color matrix mask options.

    nsecs_t timeBefore = systemTime();
    for (uint32_t keyVal = 0; keyVal <= keyMask; keyVal++) {
        Key shaderKey;
        shaderKey.set(keyMask, keyVal);
        uint32_t tex = shaderKey.getTextureTarget();
        if (tex != Key::TEXTURE_OFF &&
            tex != Key::TEXTURE_EXT &&
            tex != Key::TEXTURE_2D) {
            continue;
        }
        Program* program = mCache.valueFor(shaderKey);
        if (program == NULL) {
            program = generateProgram(shaderKey);
            mCache.add(shaderKey, program);
            shaderCount++;
        }
    }
    nsecs_t timeAfter = systemTime();
    float compileTimeMs = static_cast<float>(timeAfter - timeBefore) / 1.0E6;
    ALOGD("shader cache generated - %u shaders in %f ms\n", shaderCount, compileTimeMs);
}

ProgramCache::Key ProgramCache::computeKey(const Description& description) {
    Key needs;
#ifdef ENABLE_VR
    needs.set(Key::TEXTURE_MASK,
            !description.mTextureEnabled ? Key::TEXTURE_OFF :
            description.mTexture.getTextureTarget() == GL_TEXTURE_EXTERNAL_OES ? Key::TEXTURE_EXT :
            description.mTexture.getTextureTarget() == GL_TEXTURE_2D           ? Key::TEXTURE_2D :
            Key::TEXTURE_OFF)
    .set(Key::PLANE_ALPHA_MASK,
            (description.mPlaneAlpha < 1) ? Key::PLANE_ALPHA_LT_ONE : Key::PLANE_ALPHA_EQ_ONE)
    .set(Key::BLEND_MASK,
            description.mPremultipliedAlpha ? Key::BLEND_PREMULT : Key::BLEND_NORMAL)
    .set(Key::OPACITY_MASK,
            description.mOpaque ? Key::OPACITY_OPAQUE : Key::OPACITY_TRANSLUCENT)
    .set(Key::COLOR_MATRIX_MASK,
            description.mColorMatrixEnabled ? Key::COLOR_MATRIX_ON :  Key::COLOR_MATRIX_OFF)
    .set(Key::DEFORMATION_MASK,
            description.mDeformEnabled ? Key::DEFORMATION_ON : Key::DEFORMATION_OFF)
    .set(Key::DISPERSION_MASK,
            description.mDispersionEnabled ? Key::DISPERSION_ON : Key::DISPERSION_OFF);
#else
    needs.set(Key::TEXTURE_MASK,
            !description.mTextureEnabled ? Key::TEXTURE_OFF :
            description.mTexture.getTextureTarget() == GL_TEXTURE_EXTERNAL_OES ? Key::TEXTURE_EXT :
            description.mTexture.getTextureTarget() == GL_TEXTURE_2D           ? Key::TEXTURE_2D :
            Key::TEXTURE_OFF)
    .set(Key::PLANE_ALPHA_MASK,
            (description.mPlaneAlpha < 1) ? Key::PLANE_ALPHA_LT_ONE : Key::PLANE_ALPHA_EQ_ONE)
    .set(Key::BLEND_MASK,
            description.mPremultipliedAlpha ? Key::BLEND_PREMULT : Key::BLEND_NORMAL)
    .set(Key::OPACITY_MASK,
            description.mOpaque ? Key::OPACITY_OPAQUE : Key::OPACITY_TRANSLUCENT)
    .set(Key::COLOR_MATRIX_MASK,
            description.mColorMatrixEnabled ? Key::COLOR_MATRIX_ON :  Key::COLOR_MATRIX_OFF);
#endif

    return needs;
}

String8 ProgramCache::generateVertexShader(const Key& needs) {
    Formatter vs;
    if (needs.isTexturing()) {
        vs  << "attribute vec4 texCoords;"
            << "attribute vec4 texCoords_r;"
            << "attribute vec4 texCoords_g;"
            << "attribute vec4 texCoords_b;"

            << "varying vec2 outTexCoords_r;"
            << "varying vec2 outTexCoords_g;"
            << "varying vec2 outTexCoords_b;"

            << "varying vec2 outTexCoords;";
    }
    vs << "attribute vec4 position;"
       << "uniform mat4 projection;"
       << "uniform mat4 texture;"
       << "void main(void) {" << indent
       << "gl_Position = projection * position;";
    if (needs.isTexturing()) {
        vs << "outTexCoords = (texture * texCoords).st;"
           << "outTexCoords_r = (texture * texCoords_r).st;"
           << "outTexCoords_g = (texture * texCoords_g).st;"
           << "outTexCoords_b = (texture * texCoords_b).st;";
    }
    vs << dedent << "}";
    return vs.getString();
}

String8 ProgramCache::generateFragmentShader(const Key& needs) {
    Formatter fs;
    if (needs.getTextureTarget() == Key::TEXTURE_EXT) {
        fs << "#extension GL_OES_EGL_image_external : require";
    }

    // default precision is required-ish in fragment shaders
    fs << "precision mediump float;";
    if (needs.getTextureTarget() == Key::TEXTURE_EXT) {
        fs << "uniform samplerExternalOES sampler;"
           << "uniform sampler2D FogBorder;"
           << "varying vec2 outTexCoords;"
           << "varying vec2 outTexCoords_r;"
           << "varying vec2 outTexCoords_g;"
           << "varying vec2 outTexCoords_b;";

    } else if (needs.getTextureTarget() == Key::TEXTURE_2D) {
        fs << "uniform sampler2D sampler;"

           << "varying vec2 outTexCoords;"
           << "varying vec2 outTexCoords_r;"
           << "varying vec2 outTexCoords_g;"
           << "varying vec2 outTexCoords_b;";

    } else if (needs.getTextureTarget() == Key::TEXTURE_OFF) {
        fs << "uniform vec4 color;";
    }
    if (needs.hasPlaneAlpha()) {
        fs << "uniform float alphaPlane;";
    }
    //if (needs.hasColorMatrix()) {
    //    fs << "uniform mat4 colorMatrix;";
    //}
    fs << "void main(void) {" << indent;
    if (needs.isTexturing()) {
#ifdef ENABLE_VR
        if(needs.hasDeform())
        {
            if(needs.hasDispersion()){
                //fs << "if(outTexCoords_r.x>=0.0 && outTexCoords_r.x<=1.0 && outTexCoords_r.y>=0.0 && outTexCoords_r.y<=1.0)";
                //fs << "if(outTexCoords_g.x>=0.0 && outTexCoords_g.x<=1.0 && outTexCoords_g.y>=0.0 && outTexCoords_g.y<=1.0)";
                //fs << "if(outTexCoords_b.y>=0.0 && outTexCoords_b.y<=1.0){";
                fs << "float scale = 20.0;";
                fs << "float fade_top    = clamp(       outTexCoords_r.y  * scale,0.0,1.0);";
                fs << "float fade_bottom = clamp((1.0 - outTexCoords_r.y) * scale,0.0,1.0);";
                fs << "float fade_left   = clamp(       outTexCoords_r.x  * scale,0.0,1.0);";
                fs << "float fade_right  = clamp((1.0 - outTexCoords_r.x) * scale,0.0,1.0);";
                fs << "float fade = fade_top * fade_bottom * fade_left * fade_right;";

                fs << "gl_FragColor.r = texture2D(sampler, outTexCoords_r).r * fade;";
                fs << "gl_FragColor.g = texture2D(sampler, outTexCoords_g).g * fade;";
                fs << "gl_FragColor.b = texture2D(sampler, outTexCoords_b).b * fade;";
                fs << "gl_FragColor.a = 1.0;";
                //fs << "}else{";
                //fs << "   gl_FragColor = vec4(0.0,0.0,0.0,0.0);";
                //fs << "}";
            }else{
                fs << "float scale = 20.0;";
                fs << "float fade_top    = clamp(       outTexCoords_r.y  * scale,0.0,1.0);";
                fs << "float fade_bottom = clamp((1.0 - outTexCoords_r.y) * scale,0.0,1.0);";
                fs << "float fade_left   = clamp(       outTexCoords_r.x  * scale,0.0,1.0);";
                fs << "float fade_right  = clamp((1.0 - outTexCoords_r.x) * scale,0.0,1.0);";
                fs << "float fade = fade_top * fade_bottom * fade_left * fade_right;";

                fs << "gl_FragColor   = texture2D(sampler, outTexCoords_r) * fade;";
                fs << "gl_FragColor.a = 1.0;";
            }
        }
        else
            fs << "   gl_FragColor = texture2D(sampler, outTexCoords);";

#else
        fs << "gl_FragColor = texture2D(sampler, outTexCoords);";
#endif
    } else {
        fs << "gl_FragColor = color;";
    }
    if (needs.isOpaque()) {
        fs << "gl_FragColor.a = 1.0;";
    }
    if (needs.hasPlaneAlpha()) {
        // modulate the alpha value with planeAlpha
        if (needs.isPremultiplied()) {
            // ... and the color too if we're premultiplied
            fs << "gl_FragColor *= alphaPlane;";
        } else {
            fs << "gl_FragColor.a *= alphaPlane;";
        }
    }

    if (needs.hasColorMatrix()) {
        if (!needs.isOpaque() && needs.isPremultiplied()) {
            // un-premultiply if needed before linearization
            fs << "gl_FragColor.rgb = gl_FragColor.rgb/gl_FragColor.a;";
        }
        fs << "gl_FragColor.rgb = pow(gl_FragColor.rgb, vec3(2.2));";
        fs << "vec4 transformed = colorMatrix * vec4(gl_FragColor.rgb, 1);";
        fs << "gl_FragColor.rgb = transformed.rgb/transformed.a;";
        fs << "gl_FragColor.rgb = pow(gl_FragColor.rgb, vec3(1.0 / 2.2));";
        if (!needs.isOpaque() && needs.isPremultiplied()) {
            // and re-premultiply if needed after gamma correction
            fs << "gl_FragColor.rgb = gl_FragColor.rgb*gl_FragColor.a;";
        }
    }

    fs << dedent << "}";
    return fs.getString();
}

Program* ProgramCache::generateProgram(const Key& needs) {
    // vertex shader
    String8 vs = generateVertexShader(needs);

    // fragment shader
    String8 fs = generateFragmentShader(needs);

    Program* program = new Program(needs, vs.string(), fs.string());
    return program;
}

void ProgramCache::useProgram(const Description& description) {
    // generate the key for the shader based on the description
    Key needs(computeKey(description));

     // look-up the program in the cache
    Program* program = mCache.valueFor(needs);
    if (program == NULL) {
        // we didn't find our program, so generate one...
        nsecs_t time = -systemTime();
        program = generateProgram(needs);
        mCache.add(needs, program);
        time += systemTime();

        //ALOGD(">>> generated new program: needs=%08X, time=%u ms (%d programs)",
        //        needs.mNeeds, uint32_t(ns2ms(time)), mCache.size());
    }
    // here we have a suitable program for this description
    if (program->isValid()) {
        program->use();
        program->setUniforms(description);
    }
}
} /* namespace android */
