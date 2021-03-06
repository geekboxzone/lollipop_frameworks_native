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
typedef unsigned int uint32_t;
typedef unsigned char uint8_t;

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#define Warp_Mesh_Resolution_X 64
#define Warp_Mesh_Resolution_Y 64
#define VR_Buffer_Stride 10
#define Screen_X 1440.0f
#define Screen_Y 2560.0f
#define Check_Width 8
#define Check_Height 8
#define Check_Len 8
#define LEFT 1
#define RIGHT 2

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <ui/Rect.h>

#include <utils/String8.h>
#include <utils/Trace.h>

#include <cutils/compiler.h>
#include <gui/ISurfaceComposer.h>
#include <math.h>
#include <cutils/properties.h>

#include "GLES20RenderEngine.h"
#include "Program.h"
#include "ProgramCache.h"
#include "Description.h"
#include "Mesh.h"
#include "Texture.h"

// ---------------------------------------------------------------------------
namespace android {
// ---------------------------------------------------------------------------
GLES20RenderEngine::GLES20RenderEngine() :
        mVpWidth(0),
        mVpHeight(0),
        VRMeshBuffer(0),
        tname(0),
        name(0),
        leftFbo(0),
        rightFbo(0),
        leftTex(0),
        rightTex(0),
        useRightFBO(false),
        context(NULL),
        leftCheck(NULL),
        rightCheck(NULL),
        is3dApp(false),
        checkRate(100),
        checkBegin(0),
        checkLeftTex(0),
        checkRightTex(0),
        checkLeftFBO(0),
        checkRightFBO(0)
{
#ifdef ENABLE_VR
    initVRInfoTable();
    mVRInfoTable.VRMeshBuffer = genVRMeshBuffer(Screen_X,Screen_Y);
#endif

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &mMaxTextureSize);
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS, mMaxViewportDims);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);

    struct pack565 {
        inline uint16_t operator() (int r, int g, int b) const {
            return (r<<11)|(g<<5)|b;
        }
    } pack565;

    const uint16_t protTexData[] = { 0 };
    glGenTextures(1, &mProtectedTexName);
    glBindTexture(GL_TEXTURE_2D, mProtectedTexName);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0,
            GL_RGB, GL_UNSIGNED_SHORT_5_6_5, protTexData);

    //mColorBlindnessCorrection = M;
}

GLES20RenderEngine::~GLES20RenderEngine() {
}


size_t GLES20RenderEngine::getMaxTextureSize() const {
    return mMaxTextureSize;
}

size_t GLES20RenderEngine::getMaxViewportDims() const {
    return
        mMaxViewportDims[0] < mMaxViewportDims[1] ?
            mMaxViewportDims[0] : mMaxViewportDims[1];
}

void GLES20RenderEngine::setViewportAndProjection(
        size_t vpw, size_t vph, Rect sourceCrop, size_t hwh, bool yswap,
        Transform::orientation_flags rotation) {

    size_t l = sourceCrop.left;
    size_t r = sourceCrop.right;

    // In GL, (0, 0) is the bottom-left corner, so flip y coordinates
    size_t t = hwh - sourceCrop.top;
    size_t b = hwh - sourceCrop.bottom;

    mat4 m;
    if (yswap) {
        m = mat4::ortho(l, r, t, b, 0, 1);
    } else {
        m = mat4::ortho(l, r, b, t, 0, 1);
    }

    // Apply custom rotation to the projection.
    float rot90InRadians = 2.0f * static_cast<float>(M_PI) / 4.0f;
    switch (rotation) {
        case Transform::ROT_0:
            break;
        case Transform::ROT_90:
            m = mat4::rotate(rot90InRadians, vec3(0,0,1)) * m;
            break;
        case Transform::ROT_180:
            m = mat4::rotate(rot90InRadians * 2.0f, vec3(0,0,1)) * m;
            break;
        case Transform::ROT_270:
            m = mat4::rotate(rot90InRadians * 3.0f, vec3(0,0,1)) * m;
            break;
        default:
            break;
    }

    glViewport(0, 0, vpw, vph);
    mState.setProjectionMatrix(m);
    mVpWidth = vpw;
    mVpHeight = vph;
}

void GLES20RenderEngine::setupLayerBlending(
    bool premultipliedAlpha, bool opaque, int alpha) {

    mState.setPremultipliedAlpha(premultipliedAlpha);
    mState.setOpaque(opaque);
    mState.setPlaneAlpha(alpha / 255.0f);

    if (alpha < 0xFF || !opaque) {
        glEnable(GL_BLEND);
       // glBlendFunc(premultipliedAlpha ? GL_ONE : GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
       glBlendFuncSeparate(premultipliedAlpha ? GL_ONE : GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    } else {
        glDisable(GL_BLEND);
    }
}

void GLES20RenderEngine::setupDimLayerBlending(int alpha) {
    mState.setPlaneAlpha(1.0f);
    mState.setPremultipliedAlpha(true);
    mState.setOpaque(false);
    mState.setColor(0, 0, 0, alpha/255.0f);
    mState.disableTexture();

    if (alpha == 0xFF) {
        glDisable(GL_BLEND);
    } else {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }
}

void GLES20RenderEngine::setupLayerTexturing(const Texture& texture) {
    GLuint target = texture.getTextureTarget();
    glBindTexture(target, texture.getTextureName());
    GLenum filter = GL_NEAREST;
    if (texture.getFiltering()) {
        filter = GL_LINEAR;
    }
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, filter);

    mState.setTexture(texture);
}

void GLES20RenderEngine::setupLayerBlackedOut() {
    glBindTexture(GL_TEXTURE_2D, mProtectedTexName);
    Texture texture(Texture::TEXTURE_2D, mProtectedTexName);
    texture.setDimensions(1, 1); // FIXME: we should get that from somewhere
    mState.setTexture(texture);
}

void GLES20RenderEngine::disableTexturing() {
    mState.disableTexture();
}

void GLES20RenderEngine::disableBlending() {
    glDisable(GL_BLEND);
}


void GLES20RenderEngine::bindImageAsFramebuffer(EGLImageKHR image,
        uint32_t* texName, uint32_t* fbName, uint32_t* status) {
    GLuint tname, name;
    // turn our EGLImage into a texture
    glGenTextures(1, &tname);
    glBindTexture(GL_TEXTURE_2D, tname);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)image);

    // create a Framebuffer Object to render into
    glGenFramebuffers(1, &name);
    glBindFramebuffer(GL_FRAMEBUFFER, name);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tname, 0);

    *status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    *texName = tname;
    *fbName = name;
}

void GLES20RenderEngine::unbindFramebuffer(uint32_t texName, uint32_t fbName) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbName);
    glDeleteTextures(1, &texName);
}

void GLES20RenderEngine::setupFillWithColor(float r, float g, float b, float a) {
    mState.setPlaneAlpha(1.0f);
    mState.setPremultipliedAlpha(true);
    mState.setOpaque(false);
    mState.setColor(r, g, b, a);
    mState.disableTexture();
    glDisable(GL_BLEND);
}

void GLES20RenderEngine::drawMesh(const Mesh& mesh) {
#ifdef ENABLE_VR
    print3dLog();
#endif
    ProgramCache::getInstance().useProgram(mState);

    if (mesh.getTexCoordsSize()) {
        glEnableVertexAttribArray(Program::texCoords);
        glVertexAttribPointer(Program::texCoords,
                mesh.getTexCoordsSize(),
                GL_FLOAT, GL_FALSE,
                mesh.getByteStride(),
                mesh.getTexCoords());
    }

    glVertexAttribPointer(Program::position,
            mesh.getVertexSize(),
            GL_FLOAT, GL_FALSE,
            mesh.getByteStride(),
            mesh.getPositions());

    glDrawArrays(mesh.getPrimitive(), 0, mesh.getVertexCount());

    if (mesh.getTexCoordsSize()) {
        glDisableVertexAttribArray(Program::texCoords);
    }

}

#ifdef ENABLE_VR
void GLES20RenderEngine::initVRInfoTable(){
    mVRInfoTable.VRMeshBuffer = 0;

    mVRInfoTable.leftFbo = 0;
    mVRInfoTable.leftTex = 0;

    mVRInfoTable.rightFbo = 0;
    mVRInfoTable.rightTex = 0;

    mVRInfoTable.checkLeftTex = 0;
    mVRInfoTable.checkLeftFBO = 0;

    mVRInfoTable.checkRightTex = 0;
    mVRInfoTable.checkRightFBO = 0;

    mVRInfoTable.checkLeftPtr = NULL;
    mVRInfoTable.checkRightPtr = NULL;

    mVRInfoTable.fboWidth  = 0;
    mVRInfoTable.fboHeight = 0;

    mVRInfoTable.is3dApp = false;
}

void GLES20RenderEngine::drawMeshLeftEye() {
    ProgramCache::getInstance().useProgram(mState);

    enableShaderTexArray();
    enableShaderVerArray(LEFT);
    glDrawArrays(Mesh::TRIANGLES, 0, Warp_Mesh_Resolution_X * Warp_Mesh_Resolution_Y * 6);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void GLES20RenderEngine::drawMeshRightEye() {
    ProgramCache::getInstance().useProgram(mState);

    enableShaderTexArray();
    enableShaderVerArray(RIGHT);
    glDrawArrays(Mesh::TRIANGLES, 0,Warp_Mesh_Resolution_X * Warp_Mesh_Resolution_Y * 6);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void GLES20RenderEngine::drawMeshLeftFBO(const Mesh& mesh) {
    //print log when per layer was drawn to fbo
    print3dLog();
    glBindFramebuffer(GL_FRAMEBUFFER, mVRInfoTable.leftFbo);
    mState.setDeform(false);
    ProgramCache::getInstance().useProgram(mState);

    if (mesh.getTexCoordsSize()) {
        glEnableVertexAttribArray(Program::texCoords);
        glVertexAttribPointer(Program::texCoords,
                mesh.getTexCoordsSize(),
                GL_FLOAT, GL_FALSE,
                mesh.getByteStride(),
                mesh.getTexCoords());
    }

    glVertexAttribPointer(Program::position,
            mesh.getVertexSize(),
            GL_FLOAT, GL_FALSE,
            mesh.getByteStride(),
            mesh.getPositions());

    glDrawArrays(mesh.getPrimitive(), 0, mesh.getVertexCount());

    if (mesh.getTexCoordsSize()) {
        glDisableVertexAttribArray(Program::texCoords);
    }
}

void GLES20RenderEngine::drawMeshRightFBO(const Mesh& mesh) {
    //print log when per layer was drawn to fbo
    print3dLog();
    glBindFramebuffer(GL_FRAMEBUFFER, mVRInfoTable.rightFbo);
    mState.setDeform(false);
    ProgramCache::getInstance().useProgram(mState);

    if (mesh.getTexCoordsSize()) {
        glEnableVertexAttribArray(Program::texCoords);
        glVertexAttribPointer(Program::texCoords,
                mesh.getTexCoordsSize(),
                GL_FLOAT, GL_FALSE,
                mesh.getByteStride(),
                mesh.getTexCoords());
    }

    glVertexAttribPointer(Program::position,
            mesh.getVertexSize(),
            GL_FLOAT, GL_FALSE,
            mesh.getByteStride(),
            mesh.getPositions());

    glDrawArrays(mesh.getPrimitive(), 0, mesh.getVertexCount());

    if (mesh.getTexCoordsSize()) {
        glDisableVertexAttribArray(Program::texCoords);
    }
}

void GLES20RenderEngine::enableShaderTexArray(){
    glBindBuffer(GL_ARRAY_BUFFER, mVRInfoTable.VRMeshBuffer);

    //rgb texCoords
    glEnableVertexAttribArray(Program::texCoords_r);
    glVertexAttribPointer(Program::texCoords_r,
        2,
        GL_FLOAT, GL_FALSE,
        VR_Buffer_Stride*sizeof(float),
        (void*)(4 * sizeof(GLfloat)));

    glEnableVertexAttribArray(Program::texCoords_g);
    glVertexAttribPointer(Program::texCoords_g,
        2,
        GL_FLOAT, GL_FALSE,
        VR_Buffer_Stride*sizeof(float),
        (void*)(6 * sizeof(GLfloat)));

    glEnableVertexAttribArray(Program::texCoords_b);
    glVertexAttribPointer(Program::texCoords_b,
        2,
        GL_FLOAT, GL_FALSE,
        VR_Buffer_Stride*sizeof(float),
        (void*)(8 * sizeof(GLfloat)));
}

void GLES20RenderEngine::enableShaderVerArray(int mode){
    glBindBuffer(GL_ARRAY_BUFFER, mVRInfoTable.VRMeshBuffer);

    if(LEFT == mode){
        glEnableVertexAttribArray(Program::position);
        glVertexAttribPointer(Program::position,
            2,
            GL_FLOAT, GL_FALSE,
            VR_Buffer_Stride*sizeof(float),
            (void*)(0 * sizeof(GLfloat)));
    }

    if(RIGHT == mode){
        glEnableVertexAttribArray(Program::position);
        glVertexAttribPointer(Program::position,
            2,
            GL_FLOAT, GL_FALSE,
            VR_Buffer_Stride*sizeof(float),
            (void*)(2 * sizeof(GLfloat)));
    }
}

vec2 GLES20RenderEngine::genDeformTex(vec2 tex,float k1,float k2){
    char value[PROPERTY_VALUE_MAX];
    property_get("sys.3d.height", value, "0.5");
    float Scale = atof(value);
    property_get("sys.3d.ipd_scale", value, "0");
    float ipdByScale = atof(value);
    if(ipdByScale < 0)
        ipdByScale  = (-1.0) * ipdByScale;

    float xyRatio = (Screen_X * Scale)/ ((Screen_Y/2) * (1.0 - 0.5 * ipdByScale));
    tex = tex - vec2(0.5);
    tex.x = tex.x * xyRatio;

    float len = length(tex);
    float r2 = pow(len,2.0);
    float r4 = pow(len,4.0);
    tex = tex * (1 + k1 * r2 + k2 * r4);

    tex.x = tex.x / xyRatio;
    tex = tex + vec2(0.5);

    return tex;
}

GLuint GLES20RenderEngine::genVRMeshBuffer(float width,float height){
    struct Vertex
    {
        vec2 left_position;
        vec2 right_position;
        vec2 uv_red;
        vec2 uv_green;
        vec2 uv_blue;
    };
    static Vertex v[(Warp_Mesh_Resolution_X + 1) * (Warp_Mesh_Resolution_Y + 1)];

    char value[PROPERTY_VALUE_MAX];
    //orientation
    property_get("sys.hwc.force3d.primary", value, "2");
    int orient = atoi(value);

    //r g b deform property set
    property_get("sys.3d.deform_red1", value, "0");
    float rk1 = atof(value);
    property_get("sys.3d.deform_red2", value, "0");
    float rk2 = atof(value);
    property_get("sys.3d.deform_green1", value, "0");
    float gk1 = atof(value);
    property_get("sys.3d.deform_green2", value, "0");
    float gk2 = atof(value);
    property_get("sys.3d.deform_blue1", value, "0");
    float bk1 = atof(value);
    property_get("sys.3d.deform_blue2", value, "0");
    float bk2 = atof(value);

    //height property set
    property_get("sys.3d.height", value, "0.5");
    float heightScale = atof(value);

    /*IPD property set
      IPD Offset priority is higher than Scale
      if Offset != 0, we set the Scale = 0*/
    property_get("sys.3d.ipd_offset", value, "0");
    float ipdByOffset = atof(value);
    property_get("sys.3d.ipd_scale", value, "0");
    float ipdByScale = atof(value);
    if(ipdByOffset!=0 && ipdByScale!=0)
    ipdByScale = 0.0f;

    float finalHeight = 0;
    float finalWidth = 0;
    float ipdMaxSize = 0;

    //size of fbo
    if(2==orient){
        ipdMaxSize = (Screen_Y/2)/10.0f;
        finalHeight = height * 0.5;
        finalWidth  = width  * heightScale;
    }
    if(1==orient){
        ipdMaxSize = (Screen_X/2)/10.0f;
        finalHeight = height * heightScale;
        finalWidth  = width  * 0.5;
    }

    // Compute vertices
    int vi = 0;
    for (int yi = 0; yi <= Warp_Mesh_Resolution_Y; yi++)
    for (int xi = 0; xi <= Warp_Mesh_Resolution_X; xi++)
        {
            float x = float(xi) / float(Warp_Mesh_Resolution_X);
            float y = float(yi) / float(Warp_Mesh_Resolution_Y);

            vec2 tex = vec2(x,y);

            //cellphone orient mode
            if(2==orient){
                //vec position's range is frome Screen_X & Screen_Y,not 0~1
                v[vi].left_position  = vec2(finalWidth*x + Screen_X * ((1 - heightScale) * 0.5),finalHeight*y);
                v[vi].right_position = vec2(finalWidth*x + Screen_X * ((1 - heightScale) * 0.5),finalHeight*y+finalHeight);

                v[vi].uv_red    = genDeformTex(tex,rk1,rk2);
                v[vi].uv_green  = genDeformTex(tex,gk1,gk2);
                v[vi].uv_blue   = genDeformTex(tex,bk1,bk2);

                //if enable IPD_Offset
                v[vi].left_position.y  = v[vi].left_position.y  + ipdMaxSize * ipdByOffset;
                v[vi].right_position.y = v[vi].right_position.y - ipdMaxSize * ipdByOffset;
                v[vi].left_position.y  = ( v[vi].left_position.y < finalHeight) ?  v[vi].left_position.y : finalHeight;
                v[vi].right_position.y = (v[vi].right_position.y > finalHeight) ? v[vi].right_position.y : finalHeight;

                //if enable IPD_Scale
                if(ipdByScale > 0){
                    float screenScale = 1.0 - 0.5 * ipdByScale;
                    v[vi].left_position.y  = v[vi].left_position.y * screenScale;
                    v[vi].right_position.y = v[vi].right_position.y * screenScale + (Screen_Y/4.0f) * ipdByScale * 2.0f;
                }
                if(ipdByScale < 0){
                    float ipdByScale_abs = (-1.0f) * ipdByScale;
                    float screenScale = 1.0 - 0.5 * ipdByScale_abs;
                    v[vi].left_position.y  = v[vi].left_position.y * screenScale + (Screen_Y/4.0f) * ipdByScale_abs;
                    v[vi].right_position.y = v[vi].right_position.y * screenScale + (Screen_Y/4.0f) * ipdByScale_abs;
                }
            }

            //tablet orient mode
            if(1==orient){
                //vec position's range is frome Screen_X & Screen_Y,not 0~1
                v[vi].left_position  = vec2(finalWidth*x ,finalHeight*y + Screen_Y * ((1 - heightScale) * 0.5));
                v[vi].right_position = vec2(finalWidth*x + finalWidth,finalHeight*y + Screen_Y * ((1 - heightScale) * 0.5));

                v[vi].uv_red    = genDeformTex(tex,rk1,rk2);
                v[vi].uv_green  = genDeformTex(tex,gk1,gk2);
                v[vi].uv_blue   = genDeformTex(tex,bk1,bk2);

                //if enable IPD_Offset
                v[vi].left_position.x  = v[vi].left_position.x  + ipdMaxSize * ipdByOffset;
                v[vi].right_position.x = v[vi].right_position.x - ipdMaxSize * ipdByOffset;
                v[vi].left_position.x  = ( v[vi].left_position.x < finalWidth) ?  v[vi].left_position.x : finalWidth;
                v[vi].right_position.x = (v[vi].right_position.x > finalWidth) ? v[vi].right_position.x : finalWidth;

                //if enable IPD_Scale
                if(ipdByScale > 0){
                    float screenScale = 1.0 - 0.5 * ipdByScale;
                    v[vi].left_position.x  = v[vi].left_position.x * screenScale;
                    v[vi].right_position.x = v[vi].right_position.x * screenScale + (Screen_X/4.0f) * ipdByScale * 2.0f;
                }
                if(ipdByScale < 0){
                    float ipdByScale_abs = (-1.0f) * ipdByScale;
                    float screenScale = 1.0 - 0.5 * ipdByScale_abs;
                    v[vi].left_position.x  = v[vi].left_position.x * screenScale + (Screen_X/4.0f) * ipdByScale_abs;
                    v[vi].right_position.x = v[vi].right_position.x * screenScale + (Screen_X/4.0f) * ipdByScale_abs;
                }
            }

            vi++;
        }

    // Generate faces from vertices
    static Vertex f[Warp_Mesh_Resolution_X * Warp_Mesh_Resolution_Y * 6];
    int fi = 0;
    for (int yi = 0; yi < Warp_Mesh_Resolution_Y; yi++)
    for (int xi = 0; xi < Warp_Mesh_Resolution_X; xi++)
    {
        Vertex v0 = v[(yi    ) * (Warp_Mesh_Resolution_X + 1) + xi    ];
        Vertex v1 = v[(yi    ) * (Warp_Mesh_Resolution_X + 1) + xi + 1];
        Vertex v2 = v[(yi + 1) * (Warp_Mesh_Resolution_X + 1) + xi + 1];
        Vertex v3 = v[(yi + 1) * (Warp_Mesh_Resolution_X + 1) + xi    ];
        f[fi++] = v0;
        f[fi++] = v1;
        f[fi++] = v2;
        f[fi++] = v2;
        f[fi++] = v3;
        f[fi++] = v0;
    }

    GLuint result = 0;
    glGenBuffers(1, &result);
    glBindBuffer(GL_ARRAY_BUFFER, result);
    glBufferData(GL_ARRAY_BUFFER, sizeof(f), f, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return result;
}

void GLES20RenderEngine::enableRightFBO(bool key){
    if(key)
        useRightFBO = true;
    else
        useRightFBO = false;
}

//judge whether we need to generate VRMeshBuffer once again or not
bool GLES20RenderEngine::checkVRPropertyChanged(){
    char value[PROPERTY_VALUE_MAX];
    property_get("sys.3d.property_update", value, "1");
    int result = atoi(value);

    if(result){
        property_set("sys.3d.property_update","0");
        return true;//re-compute
    }else
        return false;//don't re-compute
}

void GLES20RenderEngine::print3dLog(){
    char value[PROPERTY_VALUE_MAX];
    property_get("sys.3d.log", value, "0");
    int log3d = atoi(value);
    if(1==log3d){
        ALOGD("3dlog:(setStereoDraw):***3D Display X&Y:");
        ALOGD("3dlog:(setStereoDraw):  Screen_X = %f   Screen_Y = %f",Screen_X,Screen_Y);
    }
}
void GLES20RenderEngine::clearFbo(){
    glBindFramebuffer(GL_FRAMEBUFFER, mVRInfoTable.leftFbo);
    glClearColor(0,0,0,0);
    glClear(GL_COLOR_BUFFER_BIT);

    glBindFramebuffer(GL_FRAMEBUFFER, mVRInfoTable.rightFbo);
    glClearColor(0,0,0,0);
    glClear(GL_COLOR_BUFFER_BIT);

    //After we clear fbo,we must bind the original fbo to current
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GLES20RenderEngine::beginGroup(const mat4& colorTransform,int mode) {
    char value[PROPERTY_VALUE_MAX];
    property_get("sys.3d.height", value, "0.5");
    float heightScale = atof(value);

    property_get("sys.hwc.force3d.primary", value, "0");
    int orient = atoi(value);

    if(2==orient){
        mVRInfoTable.fboWidth  = Screen_X * heightScale +2;
        mVRInfoTable.fboHeight = Screen_Y * 0.5 + 2;
    }

    if(1==orient){
        mVRInfoTable.fboWidth  = Screen_X * 0.5 + 2;
        mVRInfoTable.fboHeight = Screen_Y * heightScale + 2;
    }

    //check whether need to recompute MeshBuffer
    if(checkVRPropertyChanged()){
        mVRInfoTable.VRMeshBuffer = genVRMeshBuffer(mVpWidth,mVpHeight);

        glBindTexture(GL_TEXTURE_2D, mVRInfoTable.leftTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mVRInfoTable.fboWidth, mVRInfoTable.fboHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE,0);
        glBindFramebuffer(GL_FRAMEBUFFER, mVRInfoTable.leftFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mVRInfoTable.leftTex, 0);

        glBindTexture(GL_TEXTURE_2D, rightTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mVRInfoTable.fboWidth, mVRInfoTable.fboHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE,0);
        glBindFramebuffer(GL_FRAMEBUFFER, mVRInfoTable.rightFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rightTex, 0);
    }

    //create Texture��FBO
    if(GL_FALSE == glIsFramebuffer(mVRInfoTable.leftFbo)){
        //the color out of border of texCoords
        float color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

        glGenTextures(1, &mVRInfoTable.checkLeftTex);
        glBindTexture(GL_TEXTURE_2D, mVRInfoTable.checkLeftTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, Check_Width, Check_Height, 0, GL_RGBA, GL_UNSIGNED_BYTE,0);
        glGenFramebuffers(1, &mVRInfoTable.checkLeftFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, mVRInfoTable.checkLeftFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mVRInfoTable.checkLeftTex, 0);

        glGenTextures(1, &mVRInfoTable.checkRightTex);
        glBindTexture(GL_TEXTURE_2D, mVRInfoTable.checkRightTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, Check_Width, Check_Height, 0, GL_RGBA, GL_UNSIGNED_BYTE,0);
        glGenFramebuffers(1, &mVRInfoTable.checkRightFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, mVRInfoTable.checkRightFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mVRInfoTable.checkRightTex, 0);

        /*create Left FBO
          0x812D:   GL_CLAMP_TO_BORDER
          0x1004:   GL_TEXTURE_BORDER_COLOR*/
        glGenTextures(1, &mVRInfoTable.leftTex);
        glBindTexture(GL_TEXTURE_2D, mVRInfoTable.leftTex);
        /*
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812D);
        glTexParameterfv( GL_TEXTURE_2D, 0x1004, color);
        */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mVRInfoTable.fboWidth, mVRInfoTable.fboHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE,0);
        glGenFramebuffers(1, &mVRInfoTable.leftFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, mVRInfoTable.leftFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mVRInfoTable.leftTex, 0);
        //glFramebufferTexture2D(GL_FRAMEBUFFER, 0x8CE1, GL_TEXTURE_2D, mVRInfoTable.checkLeftTex, 0);

        //create Right FBO
        glGenTextures(1, &rightTex);
        glBindTexture(GL_TEXTURE_2D, rightTex);
        /*
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812D);
        glTexParameterfv( GL_TEXTURE_2D, 0x1004, color);
        */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mVRInfoTable.fboWidth, mVRInfoTable.fboHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE,0);
        glGenFramebuffers(1, &mVRInfoTable.rightFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, mVRInfoTable.rightFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rightTex, 0);

        //Stack FBO:nothing to be used
        glGenTextures(1, &tname);
        glBindTexture(GL_TEXTURE_2D, tname);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);

        // original fbo, delete it then we get error
        glGenFramebuffers(1, &name);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, mVRInfoTable.leftFbo);
    Group group;

    group.texture = tname;
    group.fbo = name;
    group.width = mVpWidth;
    group.height = mVpHeight;

    mGroupStack.push(group);

    if(mode > 1)
    {
        group.colorTransform = colorTransform;
    }

}

void GLES20RenderEngine::endGroup(int mode) {
    const Group group(mGroupStack.top());
    mGroupStack.pop();

    // activate the previous render target
    GLuint fbo = 0;
    if (!mGroupStack.isEmpty()) {
        fbo = mGroupStack.top().fbo;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    //set Texture and State for the Shader
    Texture LeftTexture(Texture::TEXTURE_2D, mVRInfoTable.leftTex);
    LeftTexture.setDimensions(group.width, group.height);
    glBindTexture(GL_TEXTURE_2D, mVRInfoTable.leftTex);

    Texture RightTexture(Texture::TEXTURE_2D, rightTex);
    RightTexture.setDimensions(group.width, group.height);
    glBindTexture(GL_TEXTURE_2D, rightTex);

    mState.setPlaneAlpha(1.0f);
    mState.setPremultipliedAlpha(true);
    mState.setOpaque(false);
    mState.setColorMatrix(group.colorTransform);

    switch(mode)
    {
        case 1:
            mState.setDeform(true);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            break;
        case 2:
            mState.setDeform(true);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        case 3:
            mState.setColorMatrix(group.colorTransform);
            break;
        default:
            break;
    }
    glDisable(GL_BLEND);

    //whether enable dispersion
    char value[PROPERTY_VALUE_MAX];
    property_get("debug.sf.dispersion", value, "0");
    int dispersionEnabled = atoi(value);
    if(dispersionEnabled)
        mState.setDisper(true);
    else
        mState.setDisper(false);

    //draw fb for left eye
    glBindTexture(GL_TEXTURE_2D, mVRInfoTable.leftTex);
    mState.setTexture(LeftTexture);
    drawMeshLeftEye();

    //draw fb for right eye
    //enable right FBO according to the value of useRightFBO
    if(useRightFBO){
        glBindTexture(GL_TEXTURE_2D, rightTex);
        mState.setTexture(RightTexture);
        drawMeshRightEye();
    }else{
        mState.setTexture(LeftTexture);
        drawMeshRightEye();
    }

    //disable right FBO
    enableRightFBO(false);

    // reset color matrix
    mState.setColorMatrix(mat4());
    switch(mode)
    {
        case 1:
            mState.setDeform(false);
            break;
        case 2:
            mState.setDeform(false);
        case 3:
            mState.setColorMatrix(mat4());
            break;
        default:
            break;
    }
}


#else
void GLES20RenderEngine::beginGroup(const mat4& colorTransform) {
    GLuint tname, name;
    // create the texture
    glGenTextures(1, &tname);
    glBindTexture(GL_TEXTURE_2D, tname);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mVpWidth, mVpHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);

    // create a Framebuffer Object to render into
    glGenFramebuffers(1, &name);
    glBindFramebuffer(GL_FRAMEBUFFER, name);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tname, 0);

    Group group;
    group.texture = tname;
    group.fbo = name;
    group.width = mVpWidth;
    group.height = mVpHeight;

    group.colorTransform = colorTransform;

    mGroupStack.push(group);
}

void GLES20RenderEngine::endGroup() {
    const Group group(mGroupStack.top());
    mGroupStack.pop();

    // activate the previous render target
    GLuint fbo = 0;
    if (!mGroupStack.isEmpty()) {
        fbo = mGroupStack.top().fbo;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    // set our state
    Texture texture(Texture::TEXTURE_2D, group.texture);
    texture.setDimensions(group.width, group.height);
    glBindTexture(GL_TEXTURE_2D, group.texture);

    mState.setPlaneAlpha(1.0f);
    mState.setPremultipliedAlpha(true);
    mState.setOpaque(false);
    mState.setTexture(texture);
    mState.setColorMatrix(group.colorTransform);

    glDisable(GL_BLEND);

    Mesh mesh(Mesh::TRIANGLE_FAN, 4, 2, 2);
    Mesh::VertexArray<vec2> position(mesh.getPositionArray<vec2>());
    Mesh::VertexArray<vec2> texCoord(mesh.getTexCoordArray<vec2>());
    position[0] = vec2(0, 0);
    position[1] = vec2(group.width, 0);
    position[2] = vec2(group.width, group.height);
    position[3] = vec2(0, group.height);
    texCoord[0] = vec2(0, 0);
    texCoord[1] = vec2(1, 0);
    texCoord[2] = vec2(1, 1);
    texCoord[3] = vec2(0, 1);
    drawMesh(mesh);

    // reset color matrix
    mState.setColorMatrix(mat4());

    // free our fbo and texture
    glDeleteFramebuffers(1, &group.fbo);
    glDeleteTextures(1, &group.texture);
}
#endif



void GLES20RenderEngine::dump(String8& result) {
    RenderEngine::dump(result);
}

// ---------------------------------------------------------------------------
}; // namespace android
// ---------------------------------------------------------------------------

#if defined(__gl_h_)
#error "don't include gl/gl.h in this file"
#endif
