/*
Open Asset Import Library (assimp)
----------------------------------------------------------------------

Copyright (c) 2006-2017, assimp team

All rights reserved.

Redistribution and use of this software in source and binary forms,
with or without modification, are permitted provided that the
following conditions are met:

* Redistributions of source code must retain the above
copyright notice, this list of conditions and the
following disclaimer.

* Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the
following disclaimer in the documentation and/or other
materials provided with the distribution.

* Neither the name of the assimp team, nor the names of its
contributors may be used to endorse or promote products
derived from this software without specific prior
written permission of the assimp team.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

----------------------------------------------------------------------
*/

#ifndef ASSIMP_BUILD_NO_GLTF_IMPORTER

#include "glTF2Importer.h"
#include "StringComparison.h"
#include "StringUtils.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/ai_assert.h>
#include <assimp/DefaultLogger.hpp>
#include <assimp/importerdesc.h>

#include <memory>

#include "MakeVerboseFormat.h"

#include "glTF2Asset.h"
// This is included here so WriteLazyDict<T>'s definition is found.
#include "glTF2AssetWriter.h"

using namespace Assimp;
using namespace glTF2;


//
// glTF2Importer
//

static const aiImporterDesc desc = {
    "glTF2 Importer",
    "",
    "",
    "",
    aiImporterFlags_SupportTextFlavour | aiImporterFlags_SupportBinaryFlavour | aiImporterFlags_SupportCompressedFlavour
        | aiImporterFlags_LimitedSupport | aiImporterFlags_Experimental,
    0,
    0,
    0,
    0,
    "gltf glb"
};

glTF2Importer::glTF2Importer()
: BaseImporter()
, meshOffsets()
, embeddedTexIdxs()
, mScene( NULL ) {
    // empty
}

glTF2Importer::~glTF2Importer() {
    // empty
}

const aiImporterDesc* glTF2Importer::GetInfo() const
{
    return &desc;
}

bool glTF2Importer::CanRead(const std::string& pFile, IOSystem* pIOHandler, bool checkSig) const
{
    const std::string& extension = GetExtension(pFile);

    if (extension == "gltf" || extension == "glb")
        return true;

    if ((checkSig || !extension.length()) && pIOHandler) {
        char buffer[4];

        std::unique_ptr<IOStream> pStream(pIOHandler->Open(pFile));
        if (pStream && pStream->Read(buffer, sizeof(buffer), 1) == 1) {
            if (memcmp(buffer, AI_GLB_MAGIC_NUMBER, sizeof(buffer)) == 0) {
                return true; // Has GLB header
            }
            else if (memcmp(buffer, "{\r\n ", sizeof(buffer)) == 0
                    || memcmp(buffer, "{\n  ", sizeof(buffer)) == 0) {
                // seems a JSON file, and we're the only format that can read them
                return true;
            }
        }
    }

    return false;
}



//static void CopyValue(const glTF2::vec3& v, aiColor3D& out)
//{
//    out.r = v[0]; out.g = v[1]; out.b = v[2];
//}

static void CopyValue(const glTF2::vec4& v, aiColor4D& out)
{
    out.r = v[0]; out.g = v[1]; out.b = v[2]; out.a = v[3];
}

static void CopyValue(const glTF2::vec4& v, aiColor3D& out)
{
    out.r = v[0]; out.g = v[1]; out.b = v[2];
}

static void CopyValue(const glTF2::vec3& v, aiColor4D& out)
{
    out.r = v[0]; out.g = v[1]; out.b = v[2]; out.a = 1.0;
}

static void CopyValue(const glTF2::vec3& v, aiVector3D& out)
{
    out.x = v[0]; out.y = v[1]; out.z = v[2];
}

static void CopyValue(const glTF2::vec4& v, aiQuaternion& out)
{
    out.x = v[0]; out.y = v[1]; out.z = v[2]; out.w = v[3];
}

static void CopyValue(const glTF2::mat4& v, aiMatrix4x4& o)
{
    o.a1 = v[ 0]; o.b1 = v[ 1]; o.c1 = v[ 2]; o.d1 = v[ 3];
    o.a2 = v[ 4]; o.b2 = v[ 5]; o.c2 = v[ 6]; o.d2 = v[ 7];
    o.a3 = v[ 8]; o.b3 = v[ 9]; o.c3 = v[10]; o.d3 = v[11];
    o.a4 = v[12]; o.b4 = v[13]; o.c4 = v[14]; o.d4 = v[15];
}

inline void SetMaterialColorProperty(Asset& r, vec4& prop, aiMaterial* mat, const char* pKey, unsigned int type, unsigned int idx)
{
    aiColor4D col;
    CopyValue(prop, col);
    mat->AddProperty(&col, 1, pKey, type, idx);
}

inline void SetMaterialColorProperty(Asset& r, vec3& prop, aiMaterial* mat, const char* pKey, unsigned int type, unsigned int idx)
{
    vec4 prop4;

    prop4[0] = prop[0];
    prop4[1] = prop[1];
    prop4[2] = prop[2];
    prop4[3] = 1;

    return SetMaterialColorProperty(r, prop4, mat, pKey, type, idx);
}

inline void SetMaterialTextureProperty(std::vector<int>& embeddedTexIdxs, Asset& r, glTF2::TextureInfo prop, aiMaterial* mat, aiTextureType texType, unsigned int texSlot = 0)
{
    if (prop.texture && prop.texture->source) {
        aiString uri(prop.texture->source->uri);

        int texIdx = embeddedTexIdxs[prop.texture->source.GetIndex()];
        if (texIdx != -1) { // embedded
            // setup texture reference string (copied from ColladaLoader::FindFilenameForEffectTexture)
            uri.data[0] = '*';
            uri.length = 1 + ASSIMP_itoa10(uri.data + 1, MAXLEN - 1, texIdx);
        }

        mat->AddProperty(&uri, AI_MATKEY_TEXTURE(texType, texSlot));

        const char *texCoordName = (std::string(_AI_MATKEY_TEXTURE_BASE) + ".texCoord").c_str();
        mat->AddProperty(&prop.texCoord, 1, texCoordName, texType, texSlot);
    }
}

void glTF2Importer::ImportMaterials(glTF2::Asset& r)
{
    mScene->mNumMaterials = unsigned(r.materials.Size());
    mScene->mMaterials = new aiMaterial*[mScene->mNumMaterials];

    for (unsigned int i = 0; i < mScene->mNumMaterials; ++i) {
        aiMaterial* aimat = mScene->mMaterials[i] = new aiMaterial();

        Material& mat = r.materials[i];

        aiString str(mat.id);
        aimat->AddProperty(&str, AI_MATKEY_NAME);

        SetMaterialColorProperty(r, mat.pbrMetallicRoughness.baseColorFactor, aimat, AI_MATKEY_COLOR_DIFFUSE);
        SetMaterialTextureProperty(embeddedTexIdxs, r, mat.pbrMetallicRoughness.baseColorTexture, aimat, aiTextureType_DIFFUSE);
        SetMaterialTextureProperty(embeddedTexIdxs, r, mat.pbrMetallicRoughness.metallicRoughnessTexture, aimat, aiTextureType_UNKNOWN);
        aimat->AddProperty(&mat.pbrMetallicRoughness.metallicFactor, 1, "$mat.gltf.pbrMetallicRoughness.metallicFactor");
        aimat->AddProperty(&mat.pbrMetallicRoughness.roughnessFactor, 1, "$mat.gltf.pbrMetallicRoughness.roughnessFactor");

        SetMaterialTextureProperty(embeddedTexIdxs, r, mat.normalTexture, aimat, aiTextureType_NORMALS);
        SetMaterialTextureProperty(embeddedTexIdxs, r, mat.occlusionTexture, aimat, aiTextureType_LIGHTMAP);
        SetMaterialTextureProperty(embeddedTexIdxs, r, mat.emissiveTexture, aimat, aiTextureType_EMISSIVE);
        SetMaterialColorProperty(r, mat.emissiveFactor, aimat, AI_MATKEY_COLOR_EMISSIVE);

        aimat->AddProperty(&mat.doubleSided, 1, AI_MATKEY_TWOSIDED);
        aimat->AddProperty(&mat.alphaMode, 1, "$mat.gltf.alphaMode");
        aimat->AddProperty(&mat.alphaCutoff, 1, "$mat.gltf.alphaCutoff");

        //pbrSpecularGlossiness
        if (mat.pbrSpecularGlossiness.on) {
            aimat->AddProperty(&mat.pbrSpecularGlossiness.on, 1, "$mat.gltf.pbrSpecularGlossiness.on");
            SetMaterialColorProperty(r, mat.pbrSpecularGlossiness.diffuseFactor, aimat, "$clr.diffuse", 0, 1);
            SetMaterialColorProperty(r, mat.pbrSpecularGlossiness.specularFactor, aimat, "$clr.specular", 0, 1);
            aimat->AddProperty(&mat.pbrSpecularGlossiness.glossinessFactor, 1, "$mat.gltf.pbrSpecularGlossiness.glossinessFactor");
            SetMaterialTextureProperty(embeddedTexIdxs, r, mat.pbrSpecularGlossiness.diffuseTexture, aimat, aiTextureType_DIFFUSE, 1);
            SetMaterialTextureProperty(embeddedTexIdxs, r, mat.pbrSpecularGlossiness.specularGlossinessTexture, aimat, aiTextureType_UNKNOWN, 1);
        }
    }
}


static inline void SetFace(aiFace& face, int a)
{
    face.mNumIndices = 1;
    face.mIndices = new unsigned int[1];
    face.mIndices[0] = a;
}

static inline void SetFace(aiFace& face, int a, int b)
{
    face.mNumIndices = 2;
    face.mIndices = new unsigned int[2];
    face.mIndices[0] = a;
    face.mIndices[1] = b;
}

static inline void SetFace(aiFace& face, int a, int b, int c)
{
    face.mNumIndices = 3;
    face.mIndices = new unsigned int[3];
    face.mIndices[0] = a;
    face.mIndices[1] = b;
    face.mIndices[2] = c;
}

static inline bool CheckValidFacesIndices(aiFace* faces, unsigned nFaces, unsigned nVerts)
{
    for (unsigned i = 0; i < nFaces; ++i) {
        for (unsigned j = 0; j < faces[i].mNumIndices; ++j) {
            unsigned idx = faces[i].mIndices[j];
            if (idx >= nVerts)
                return false;
        }
    }
    return true;
}

void glTF2Importer::ImportMeshes(glTF2::Asset& r)
{
    std::vector<aiMesh*> meshes;

    unsigned int k = 0;

    for (unsigned int m = 0; m < r.meshes.Size(); ++m) {
        Mesh& mesh = r.meshes[m];

		meshOffsets.push_back(k);
        k += unsigned(mesh.primitives.size());

        for (unsigned int p = 0; p < mesh.primitives.size(); ++p) {
            Mesh::Primitive& prim = mesh.primitives[p];

            aiMesh* aim = new aiMesh();
            meshes.push_back(aim);

            aim->mName = mesh.name.empty() ? mesh.id : mesh.name;

            if (mesh.primitives.size() > 1) {
                size_t& len = aim->mName.length;
                aim->mName.data[len] = '-';
                len += 1 + ASSIMP_itoa10(aim->mName.data + len + 1, unsigned(MAXLEN - len - 1), p);
            }

            switch (prim.mode) {
                case PrimitiveMode_POINTS:
                    aim->mPrimitiveTypes |= aiPrimitiveType_POINT;
                    break;

                case PrimitiveMode_LINES:
                case PrimitiveMode_LINE_LOOP:
                case PrimitiveMode_LINE_STRIP:
                    aim->mPrimitiveTypes |= aiPrimitiveType_LINE;
                    break;

                case PrimitiveMode_TRIANGLES:
                case PrimitiveMode_TRIANGLE_STRIP:
                case PrimitiveMode_TRIANGLE_FAN:
                    aim->mPrimitiveTypes |= aiPrimitiveType_TRIANGLE;
                    break;
            }

            Mesh::Primitive::Attributes& attr = prim.attributes;

			if (attr.position.size() > 0 && attr.position[0]) {
                aim->mNumVertices = attr.position[0]->count;
                attr.position[0]->ExtractData(aim->mVertices);
			}

			if (attr.normal.size() > 0 && attr.normal[0]) attr.normal[0]->ExtractData(aim->mNormals);

            for (size_t tc = 0; tc < attr.texcoord.size() && tc < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++tc) {
                attr.texcoord[tc]->ExtractData(aim->mTextureCoords[tc]);
                aim->mNumUVComponents[tc] = attr.texcoord[tc]->GetNumComponents();

                aiVector3D* values = aim->mTextureCoords[tc];
                for (unsigned int i = 0; i < aim->mNumVertices; ++i) {
                    values[i].y = 1 - values[i].y; // Flip Y coords
                }
            }


            if (prim.indices) {
				aiFace* faces = 0;
                unsigned int nFaces = 0;

                unsigned int count = prim.indices->count;

                Accessor::Indexer data = prim.indices->GetIndexer();
                ai_assert(data.IsValid());

                switch (prim.mode) {
                    case PrimitiveMode_POINTS: {
                        nFaces = count;
                        faces = new aiFace[nFaces];
                        for (unsigned int i = 0; i < count; ++i) {
                            SetFace(faces[i], data.GetUInt(i));
                        }
                        break;
                    }

                    case PrimitiveMode_LINES: {
                        nFaces = count / 2;
                        faces = new aiFace[nFaces];
                        for (unsigned int i = 0; i < count; i += 2) {
                            SetFace(faces[i / 2], data.GetUInt(i), data.GetUInt(i + 1));
                        }
                        break;
                    }

                    case PrimitiveMode_LINE_LOOP:
                    case PrimitiveMode_LINE_STRIP: {
                        nFaces = count - ((prim.mode == PrimitiveMode_LINE_STRIP) ? 1 : 0);
                        faces = new aiFace[nFaces];
                        SetFace(faces[0], data.GetUInt(0), data.GetUInt(1));
                        for (unsigned int i = 2; i < count; ++i) {
                            SetFace(faces[i - 1], faces[i - 2].mIndices[1], data.GetUInt(i));
                        }
                        if (prim.mode == PrimitiveMode_LINE_LOOP) { // close the loop
                            SetFace(faces[count - 1], faces[count - 2].mIndices[1], faces[0].mIndices[0]);
                        }
                        break;
                    }

                    case PrimitiveMode_TRIANGLES: {
                        nFaces = count / 3;
                        faces = new aiFace[nFaces];
                        for (unsigned int i = 0; i < count; i += 3) {
                            SetFace(faces[i / 3], data.GetUInt(i), data.GetUInt(i + 1), data.GetUInt(i + 2));
                        }
                        break;
                    }
                    case PrimitiveMode_TRIANGLE_STRIP: {
                        nFaces = count - 2;
                        faces = new aiFace[nFaces];
                        SetFace(faces[0], data.GetUInt(0), data.GetUInt(1), data.GetUInt(2));
                        for (unsigned int i = 3; i < count; ++i) {
                            SetFace(faces[i - 2], faces[i - 1].mIndices[1], faces[i - 1].mIndices[2], data.GetUInt(i));
                        }
                        break;
                    }
                    case PrimitiveMode_TRIANGLE_FAN:
                        nFaces = count - 2;
                        faces = new aiFace[nFaces];
                        SetFace(faces[0], data.GetUInt(0), data.GetUInt(1), data.GetUInt(2));
                        for (unsigned int i = 3; i < count; ++i) {
                            SetFace(faces[i - 2], faces[0].mIndices[0], faces[i - 1].mIndices[2], data.GetUInt(i));
                        }
                        break;
                }

                if (faces) {
                    aim->mFaces = faces;
                    aim->mNumFaces = nFaces;
                    ai_assert(CheckValidFacesIndices(faces, nFaces, aim->mNumVertices));
                }
            }


            if (prim.material) {
                aim->mMaterialIndex = prim.material.GetIndex();
            }
        }
    }

    meshOffsets.push_back(k);

    CopyVector(meshes, mScene->mMeshes, mScene->mNumMeshes);
}

void glTF2Importer::ImportCameras(glTF2::Asset& r)
{
    if (!r.cameras.Size()) return;

    mScene->mNumCameras = r.cameras.Size();
    mScene->mCameras = new aiCamera*[r.cameras.Size()];

    for (size_t i = 0; i < r.cameras.Size(); ++i) {
        Camera& cam = r.cameras[i];

        aiCamera* aicam = mScene->mCameras[i] = new aiCamera();

        if (cam.type == Camera::Perspective) {

            aicam->mAspect        = cam.cameraProperties.perspective.aspectRatio;
            aicam->mHorizontalFOV = cam.cameraProperties.perspective.yfov * aicam->mAspect;
            aicam->mClipPlaneFar  = cam.cameraProperties.perspective.zfar;
            aicam->mClipPlaneNear = cam.cameraProperties.perspective.znear;
        }
        else {
            // assimp does not support orthographic cameras
        }
    }
}

aiNode* ImportNode(aiScene* pScene, glTF2::Asset& r, std::vector<unsigned int>& meshOffsets, glTF2::Ref<glTF2::Node>& ptr)
{
    Node& node = *ptr;

    aiNode* ainode = new aiNode(node.id);

    if (!node.children.empty()) {
        ainode->mNumChildren = unsigned(node.children.size());
        ainode->mChildren = new aiNode*[ainode->mNumChildren];

        for (unsigned int i = 0; i < ainode->mNumChildren; ++i) {
            aiNode* child = ImportNode(pScene, r, meshOffsets, node.children[i]);
            child->mParent = ainode;
            ainode->mChildren[i] = child;
        }
    }

    aiMatrix4x4& matrix = ainode->mTransformation;
    if (node.matrix.isPresent) {
        CopyValue(node.matrix.value, matrix);
    }
    else {
        if (node.translation.isPresent) {
            aiVector3D trans;
            CopyValue(node.translation.value, trans);
            aiMatrix4x4 t;
            aiMatrix4x4::Translation(trans, t);
            matrix = t * matrix;
        }

        if (node.scale.isPresent) {
            aiVector3D scal(1.f);
            CopyValue(node.scale.value, scal);
            aiMatrix4x4 s;
            aiMatrix4x4::Scaling(scal, s);
            matrix = s * matrix;
        }


        if (node.rotation.isPresent) {
            aiQuaternion rot;
            CopyValue(node.rotation.value, rot);
            matrix = aiMatrix4x4(rot.GetMatrix()) * matrix;
        }
    }

    if (node.mesh) {
        ainode->mNumMeshes = 1;
        ainode->mMeshes = new unsigned int[1];

        int k = 0;
        int idx = node.mesh.GetIndex();

        for (unsigned int j = meshOffsets[idx]; j < meshOffsets[idx + 1]; ++j, ++k) {
            ainode->mMeshes[k] = j;
        }
    }

    if (node.camera) {
        pScene->mCameras[node.camera.GetIndex()]->mName = ainode->mName;
    }

    return ainode;
}

void glTF2Importer::ImportNodes(glTF2::Asset& r)
{
    if (!r.scene) return;

    std::vector< Ref<Node> > rootNodes = r.scene->nodes;

    // The root nodes
    unsigned int numRootNodes = unsigned(rootNodes.size());
    if (numRootNodes == 1) { // a single root node: use it
        mScene->mRootNode = ImportNode(mScene, r, meshOffsets, rootNodes[0]);
    }
    else if (numRootNodes > 1) { // more than one root node: create a fake root
        aiNode* root = new aiNode("ROOT");
        root->mChildren = new aiNode*[numRootNodes];
        for (unsigned int i = 0; i < numRootNodes; ++i) {
            aiNode* node = ImportNode(mScene, r, meshOffsets, rootNodes[i]);
            node->mParent = root;
            root->mChildren[root->mNumChildren++] = node;
        }
        mScene->mRootNode = root;
    }

    //if (!mScene->mRootNode) {
    //  mScene->mRootNode = new aiNode("EMPTY");
    //}
}

void glTF2Importer::ImportEmbeddedTextures(glTF2::Asset& r)
{
    embeddedTexIdxs.resize(r.images.Size(), -1);

    int numEmbeddedTexs = 0;
    for (size_t i = 0; i < r.images.Size(); ++i) {
        if (r.images[i].HasData())
            numEmbeddedTexs += 1;
    }

    if (numEmbeddedTexs == 0)
        return;

    mScene->mTextures = new aiTexture*[numEmbeddedTexs];

    // Add the embedded textures
    for (size_t i = 0; i < r.images.Size(); ++i) {
        Image img = r.images[i];
        if (!img.HasData()) continue;

        int idx = mScene->mNumTextures++;
        embeddedTexIdxs[i] = idx;

        aiTexture* tex = mScene->mTextures[idx] = new aiTexture();

        size_t length = img.GetDataLength();
        void* data = img.StealData();

        tex->mWidth = static_cast<unsigned int>(length);
        tex->mHeight = 0;
        tex->pcData = reinterpret_cast<aiTexel*>(data);

        if (!img.mimeType.empty()) {
            const char* ext = strchr(img.mimeType.c_str(), '/') + 1;
            if (ext) {
                if (strcmp(ext, "jpeg") == 0) ext = "jpg";

                size_t len = strlen(ext);
                if (len <= 3) {
                    strcpy(tex->achFormatHint, ext);
                }
            }
        }
    }
}

void glTF2Importer::InternReadFile(const std::string& pFile, aiScene* pScene, IOSystem* pIOHandler) {

    this->mScene = pScene;

    // read the asset file
    glTF2::Asset asset(pIOHandler);
    asset.Load(pFile, GetExtension(pFile) == "glb");


    //
    // Copy the data out
    //



    ImportEmbeddedTextures(asset);
    ImportMaterials(asset);

    ImportMeshes(asset);

    ImportCameras(asset);

    ImportNodes(asset);

    // TODO: it does not split the loaded vertices, should it?
    //pScene->mFlags |= AI_SCENE_FLAGS_NON_VERBOSE_FORMAT;
	MakeVerboseFormatProcess process;
    process.Execute(pScene);


    if (pScene->mNumMeshes == 0) {
        pScene->mFlags |= AI_SCENE_FLAGS_INCOMPLETE;
    }
}

#endif // ASSIMP_BUILD_NO_GLTF_IMPORTER

