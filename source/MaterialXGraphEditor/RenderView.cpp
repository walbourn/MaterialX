//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#include <MaterialXGraphEditor/RenderView.h>

#include "MaterialXRenderGlsl/GLTextureHandler.h"
#include <MaterialXRenderGlsl/GLUtil.h>
#include <MaterialXRenderGlsl/External/Glad/glad.h>

#include <MaterialXRender/CgltfLoader.h>
#include <MaterialXRender/Harmonics.h>
#include <MaterialXRender/OiioImageLoader.h>
#include <MaterialXRender/ShaderRenderer.h>
#include <MaterialXRender/StbImageLoader.h>
#include <MaterialXRender/TinyObjLoader.h>

#include <MaterialXGenShader/DefaultColorManagementSystem.h>

#include <MaterialXFormat/Environ.h>
#include <MaterialXFormat/Util.h>

#include <iostream>

#include <GLFW/glfw3.h>

const mx::Vector3 DEFAULT_CAMERA_POSITION(0.0f, 0.0f, 5.0f);
const float DEFAULT_CAMERA_VIEW_ANGLE = 45.0f;
const float DEFAULT_CAMERA_ZOOM = 1.f;

const int SHADOW_MAP_SIZE = 2048;
const int IRRADIANCE_MAP_WIDTH = 256;
const int IRRADIANCE_MAP_HEIGHT = 128;

const float ORTHO_VIEW_DISTANCE = 1000.0f;
const float ORTHO_PROJECTION_HEIGHT = 1.8f;

const std::string DIR_LIGHT_NODE_CATEGORY = "directional_light";
const std::string IRRADIANCE_MAP_FOLDER = "irradiance";

const float IDEAL_MESH_SPHERE_RADIUS = 2.0f;

const float PI = std::acos(-1.0f);

// this is mostly taken from MaterialXView Viewer.cpp but only a subset of the functions with some changes and additions

void applyModifiers(mx::DocumentPtr doc, const DocumentModifiers& modifiers)
{
    for (mx::ElementPtr elem : doc->traverseTree())
    {
        if (modifiers.remapElements.count(elem->getCategory()))
        {
            elem->setCategory(modifiers.remapElements.at(elem->getCategory()));
        }
        if (modifiers.remapElements.count(elem->getName()))
        {
            elem->setName(modifiers.remapElements.at(elem->getName()));
        }
        mx::StringVec attrNames = elem->getAttributeNames();
        for (const std::string& attrName : attrNames)
        {
            if (modifiers.remapElements.count(elem->getAttribute(attrName)))
            {
                elem->setAttribute(attrName, modifiers.remapElements.at(elem->getAttribute(attrName)));
            }
        }
        if (elem->hasFilePrefix() && !modifiers.filePrefixTerminator.empty())
        {
            std::string filePrefix = elem->getFilePrefix();
            if (!mx::stringEndsWith(filePrefix, modifiers.filePrefixTerminator))
            {
                elem->setFilePrefix(filePrefix + modifiers.filePrefixTerminator);
            }
        }
        std::vector<mx::ElementPtr> children = elem->getChildren();
        for (mx::ElementPtr child : children)
        {
            if (modifiers.skipElements.count(child->getCategory()) ||
                modifiers.skipElements.count(child->getName()))
            {
                elem->removeChild(child->getName());
            }
        }
    }

    // Remap references to unimplemented shader nodedefs.
    for (mx::NodePtr materialNode : doc->getMaterialNodes())
    {
        for (mx::NodePtr shader : getShaderNodes(materialNode))
        {
            mx::NodeDefPtr nodeDef = shader->getNodeDef();
            if (nodeDef && !nodeDef->getImplementation())
            {
                std::vector<mx::NodeDefPtr> altNodeDefs = doc->getMatchingNodeDefs(nodeDef->getNodeString());
                for (mx::NodeDefPtr altNodeDef : altNodeDefs)
                {
                    if (altNodeDef->getImplementation())
                    {
                        shader->setNodeDefString(altNodeDef->getName());
                    }
                }
            }
        }
    }

    // Remap unsupported texture coordinate indices.
    for (mx::ElementPtr elem : doc->traverseTree())
    {
        mx::NodePtr node = elem->asA<mx::Node>();
        if (node && node->getCategory() == "texcoord")
        {
            mx::InputPtr index = node->getInput("index");
            mx::ValuePtr value = index ? index->getValue() : nullptr;
            if (value && value->isA<int>() && value->asA<int>() != 0)
            {
                index->setValue(0);
            }
        }
    }
}

RenderView::RenderView(const std::string& materialFilename,
                       const std::string& meshFilename,
                       const std::string& envRadianceFilename,
                       const mx::FileSearchPath& searchPath,
                       const mx::FilePathVec& libraryFolders,
                       unsigned int screenWidth,
                       unsigned int screenHeight) :
    _textureID(0),
    _pixelRatio(1.0f),
    _screenWidth(screenWidth),
    _screenHeight(screenHeight),
    _renderFrame(nullptr),
    _materialFilename(materialFilename),
    _meshFilename(meshFilename),
    _envRadianceFilename(envRadianceFilename),
    _searchPath(searchPath),
    _libraryFolders(libraryFolders),
    _meshScale(1.0f),
    _cameraPosition(DEFAULT_CAMERA_POSITION),
    _cameraUp(0.0f, 1.0f, 0.0f),
    _cameraViewAngle(DEFAULT_CAMERA_VIEW_ANGLE),
    _cameraNearDist(0.05f),
    _cameraFarDist(5000.0f),
    _cameraZoom(DEFAULT_CAMERA_ZOOM),
    _userCameraEnabled(true),
    _userTranslationActive(false),
    _lightRotation(0.0f),
    _shadowSoftness(1),
    _ambientOcclusionGain(0.6f),
    _selectedGeom(0),
    _selectedMaterial(0),
    _viewCamera(mx::Camera::create()),
    _envCamera(mx::Camera::create()),
    _shadowCamera(mx::Camera::create()),
    _lightHandler(mx::LightHandler::create()),
    _genContext(mx::GlslShaderGenerator::create()),
    _unitRegistry(mx::UnitConverterRegistry::create()),
    _splitByUdims(true),
    _mergeMaterials(false),
    _materialCompilation(false),
    _renderTransparency(true),
    _renderDoubleSided(true),
    _captureRequested(false),
    _exitRequested(false)
{
    // Resolve input filenames, taking both the provided search path and
    // current working directory into account.
    mx::FileSearchPath localSearchPath = searchPath;
    localSearchPath.append(mx::FilePath::getCurrentPath());
    _materialFilename = localSearchPath.find(_materialFilename);
    _meshFilename = localSearchPath.find(_meshFilename);
    _envRadianceFilename = localSearchPath.find(_envRadianceFilename);

    // Set default Glsl generator options.
    _genContext.getOptions().targetColorSpaceOverride = "lin_rec709";
    _genContext.getOptions().fileTextureVerticalFlip = true;
    _genContext.getOptions().hwShadowMap = true;
}

void RenderView::initialize()
{
    // Initialize the standard libraries and color/unit management.
    loadStandardLibraries();
    
    // Initialize image handler.
    _imageHandler = mx::GLTextureHandler::create(mx::StbImageLoader::create());
#if MATERIALX_BUILD_OIIO
    _imageHandler->addLoader(mx::OiioImageLoader::create());
#endif
    _imageHandler->setSearchPath(_searchPath);

    // Create geometry handler.
    mx::TinyObjLoaderPtr objLoader = mx::TinyObjLoader::create();
    mx::CgltfLoaderPtr gltfLoader = mx::CgltfLoader::create();
    _geometryHandler = mx::GeometryHandler::create();
    _geometryHandler->addLoader(objLoader);
    _geometryHandler->addLoader(gltfLoader);
    loadMesh(_searchPath.find(_meshFilename));

    // Initialize environment light.
    loadEnvironmentLight();

    // Initialize camera.
    initCamera();

    // Update geometry selections.
    updateGeometrySelections();

    // Load the requested material document.
    loadDocument(_materialFilename, _stdLib);

    _pixelRatio = 1.f;
}

void RenderView::assignMaterial(mx::MeshPartitionPtr geometry, mx::GlslMaterialPtr material)
{
    if (!geometry || _geometryHandler->getMeshes().empty())
    {
        return;
    }
    if (geometry == getSelectedGeometry())
    {
        setSelectedMaterial(material);
    }
    if (material)
    {
        _materialAssignments[geometry] = material;
        material->unbindGeometry();
    }
    else
    {
        _materialAssignments.erase(geometry);
    }
}

mx::FilePath RenderView::getBaseOutputPath()
{
    mx::FilePath baseFilename = _searchPath.find(_materialFilename);
    baseFilename.removeExtension();
    mx::FilePath outputPath = mx::getEnviron("MATERIALX_VIEW_OUTPUT_PATH");
    if (!outputPath.isEmpty())
    {
        baseFilename = outputPath / baseFilename.getBaseName();
    }
    return baseFilename;
}

mx::ElementPredicate RenderView::getElementPredicate()
{
    return [this](mx::ConstElementPtr elem)
    {
        if (elem->hasSourceUri())
        {
            return (_xincludeFiles.count(elem->getSourceUri()) == 0);
        }
        return true;
        };
}

void RenderView::updateGeometrySelections()
{
    _geometryList.clear();
    if (_geometryHandler->getMeshes().empty())
    {
        return;
    }
    for (auto mesh : _geometryHandler->getMeshes())
    {
        for (size_t partIndex = 0; partIndex < mesh->getPartitionCount(); partIndex++)
        {
            mx::MeshPartitionPtr part = mesh->getPartition(partIndex);
            _geometryList.push_back(part);
        }
    }

    std::vector<std::string> items;
    for (const mx::MeshPartitionPtr& part : _geometryList)
    {
        std::string geomName = part->getName();
        mx::StringVec geomSplit = mx::splitString(geomName, ":");
        if (!geomSplit.empty() && !geomSplit[geomSplit.size() - 1].empty())
        {
            geomName = geomSplit[geomSplit.size() - 1];
        }
        items.push_back(geomName);
    }

    _selectedGeom = 0;
}

void RenderView::loadMesh(const mx::FilePath& filename)
{
    _geometryHandler->clearGeometry();
    if (_geometryHandler->loadGeometry(filename))
    {
        _meshFilename = filename;
        if (_splitByUdims)
        {
            for (auto mesh : _geometryHandler->getMeshes())
            {
                mesh->splitByUdims();
            }
        }

        updateGeometrySelections();

        // Assign the selected material to all geometries.
        _materialAssignments.clear();
        mx::GlslMaterialPtr material = getSelectedMaterial();
        if (material)
        {
            for (mx::MeshPartitionPtr geom : _geometryList)
            {
                assignMaterial(geom, material);
            }
        }

        // Unbind utility materials from the previous geometry.
        if (_wireMaterial)
        {
            _wireMaterial->unbindGeometry();
        }
        if (_shadowMaterial)
        {
            _shadowMaterial->unbindGeometry();
        }
    }
}

void RenderView::loadDocument(const mx::FilePath& filename, mx::DocumentPtr libraries)
{
    _materialFilename = filename;

    // Set up read options.
    mx::XmlReadOptions readOptions;
    readOptions.readXIncludeFunction = [](mx::DocumentPtr doc, const mx::FilePath& filename,
                                          const mx::FileSearchPath& searchPath, const mx::XmlReadOptions* options)
    {
        mx::FilePath resolvedFilename = searchPath.find(filename);
        if (resolvedFilename.exists())
        {
            try
            {
                readFromXmlFile(doc, resolvedFilename, searchPath, options);
            }
            catch (mx::Exception& e)
            {
                std::cerr << "Failed to read include file: " << filename.asString() << ". " <<
                    std::string(e.what()) << std::endl;
            }
        }
        else
        {
            std::cerr << "Include file not found: " << filename.asString() << std::endl;
        }
    };

    try
    {
        // Load source document.
        mx::DocumentPtr doc = mx::createDocument();
        mx::readFromXmlFile(doc, filename, _searchPath, &readOptions);

        // Import libraries.
        doc->importLibrary(libraries);

        // Apply modifiers to the content document.
        applyModifiers(doc, _modifiers);

        // Validate the document.
        std::string message;
        if (!doc->validate(&message))
        {
            std::cerr << "*** Validation warnings for " << _materialFilename.getBaseName() << " ***" << std::endl;
            std::cerr << message;
        }

        // Update materials.
        updateMaterials(doc, nullptr);
    }
    catch (mx::ExceptionRenderError& e)
    {
        for (const std::string& error : e.errorLog())
        {
            std::cerr << error << std::endl;
        }
    }
    catch (std::exception& e)
    {
        std::cerr << e.what() << std::endl;
    }
}

void RenderView::setScrollEvent(float scrollY)
{
    if (_userCameraEnabled)
    {
        _cameraZoom = std::max(0.1f, _cameraZoom * ((scrollY > 0) ? 1.1f : 0.9f));
    }
}

void RenderView::setKeyEvent(int key)
{
    if (_userCameraEnabled)
    {
        if (key == ImGuiKey_KeypadAdd)
        {
            _cameraZoom *= 1.1f;
        }
        if (key == ImGuiKey_KeypadSubtract)
        {
            _cameraZoom = std::max(0.1f, _cameraZoom * 0.9f);
        }
    }
}

void RenderView::setMouseMotionEvent(mx::Vector2 pos)
{
    if (_viewCamera->applyArcballMotion(pos))
    {
        return;
    }
    if (_userTranslationActive)
    {
        updateCameras();

        mx::Vector3 boxMin = _geometryHandler->getMinimumBounds();
        mx::Vector3 boxMax = _geometryHandler->getMaximumBounds();
        mx::Vector3 sphereCenter = (boxMax + boxMin) / 2.0f;

        float viewZ = _viewCamera->projectToViewport(sphereCenter)[2];
        mx::Vector3 pos1 = _viewCamera->unprojectFromViewport(
            mx::Vector3(pos[0], (float) _screenWidth - pos[1], viewZ));
        mx::Vector3 pos0 = _viewCamera->unprojectFromViewport(
            mx::Vector3(_userTranslationPixel[0], (float) _screenWidth - _userTranslationPixel[1], viewZ));
        _userTranslation = _userTranslationStart + (pos1 - pos0);
    }
}

void RenderView::setMouseButtonEvent(int button, bool down, mx::Vector2 pos)
{

    if ((button == 0) && !ImGui::IsKeyPressed(GLFW_KEY_RIGHT_SHIFT) && !ImGui::IsKeyPressed(GLFW_KEY_LEFT_SHIFT))
    {
        _viewCamera->arcballButtonEvent(pos, down);
    }
    else if ((button == 1) || ((button == 0) && ImGui::IsKeyDown(GLFW_KEY_RIGHT_SHIFT)) || ((button == 0) && ImGui::IsKeyDown(GLFW_KEY_LEFT_SHIFT)))
    {
        _userTranslationStart = _userTranslation;
        _userTranslationActive = true;
        _userTranslationPixel = pos;
    }
    if ((button == 0) && !down)
    {
        _viewCamera->arcballButtonEvent(pos, false);
    }
    if (!down)
    {
        _userTranslationActive = false;
    }
}

void RenderView::setMaterial(mx::TypedElementPtr elem)
{
    // compare graph element to material in order to assign correct one
    for (mx::GlslMaterialPtr mat : _materials)
    {
        mx::TypedElementPtr telem = mat->getElement();
        if (telem->getNamePath() == elem->getNamePath())
        {
            assignMaterial(_geometryList[0], mat);
        }
    }
}

void RenderView::updateMaterials(mx::DocumentPtr doc, mx::TypedElementPtr typedElem)
{
    // Clear user data on the generator.
    _genContext.clearUserData();

    // Clear materials if merging is not requested.
    if (!_mergeMaterials)
    {
        for (mx::MeshPartitionPtr geom : _geometryList)
        {
            if (_materialAssignments.count(geom))
            {
                assignMaterial(geom, nullptr);
            }
        }
        _materials.clear();
    }

    std::vector<mx::GlslMaterialPtr> newMaterials;
    try
    {
        _materialSearchPath = mx::getSourceSearchPath(doc);

        // Apply direct lights.
        applyDirectLights(doc);

        //// Check for any udim set.
        mx::ValuePtr udimSetValue = doc->getGeomPropValue(mx::UDIM_SET_PROPERTY);

        //// Create new materials.
        if (typedElem == nullptr)
        {
            std::vector<mx::TypedElementPtr> elems;
            mx::findRenderableElements(doc, elems);
            if (elems.empty())
            {
                throw mx::Exception("No renderable elements found in " + _materialFilename.getBaseName());
            }
            else
            {
                typedElem = elems[0];
            }
        }
        mx::TypedElementPtr udimElement;
        mx::NodePtr node = typedElem->asA<mx::Node>();
        mx::NodePtr materialNode = node && node->getType() == mx::MATERIAL_TYPE_STRING ? node : nullptr;
        if (udimSetValue && udimSetValue->isA<mx::StringVec>())
        {
            for (const std::string& udim : udimSetValue->asA<mx::StringVec>())
            {
                mx::GlslMaterialPtr mat = mx::GlslMaterial::create();
                mat->setDocument(doc);
                mat->setElement(typedElem);
                mat->setMaterialNode(materialNode);
                mat->setUdim(udim);
                newMaterials.push_back(mat);

                udimElement = typedElem;
            }
        }
        else
        {
            mx::GlslMaterialPtr mat = mx::GlslMaterial::create();
            mat->setDocument(doc);
            mat->setElement(typedElem);
            mat->setMaterialNode(materialNode);
            newMaterials.push_back(mat);
        }

        if (!newMaterials.empty())
        {
            // Extend the image search path to include material source folders.
            mx::FileSearchPath extendedSearchPath = _searchPath;
            extendedSearchPath.append(_materialSearchPath);
            _imageHandler->setSearchPath(extendedSearchPath);

            // Add new materials to the global vector.
            _materials.insert(_materials.end(), newMaterials.begin(), newMaterials.end());

            mx::GlslMaterialPtr udimMaterial = nullptr;
            for (mx::GlslMaterialPtr mat : newMaterials)
            {
                // Clear cached implementations, in case libraries on the file system have changed.
                _genContext.clearNodeImplementations();

                mx::TypedElementPtr elem = mat->getElement();

                std::string udim = mat->getUdim();
                if (!udim.empty())
                {
                    if ((udimElement == elem) && udimMaterial)
                    {
                        // Reuse existing material for all udims
                        mat->copyShader(udimMaterial);
                    }
                    else
                    {
                        // Generate a shader for the new material.
                        mat->generateShader(_genContext);
                        if (udimElement == elem)
                        {
                            udimMaterial = mat;
                        }
                    }
                }
                else
                {
                    // Generate a shader for the new material.
                    mat->generateShader(_genContext);
                }

                if (materialNode)
                {
                    // Apply geometric assignments specified in the document, if any.
                    for (mx::MeshPartitionPtr part : _geometryList)
                    {
                        std::string geom = part->getName();
                        for (const std::string& id : part->getSourceNames())
                        {
                            geom += mx::ARRAY_PREFERRED_SEPARATOR + id;
                        }
                        if (!getGeometryBindings(materialNode, geom).empty())
                        {
                            assignMaterial(part, mat);
                        }
                    }

                    // Apply implicit udim assignments, if any.
                    if (!udim.empty())
                    {
                        for (mx::MeshPartitionPtr geom : _geometryList)
                        {
                            if (geom->getName() == udim)
                            {
                                assignMaterial(geom, mat);
                            }
                        }
                    }
                }
            }

            // Apply fallback assignments.
            mx::GlslMaterialPtr fallbackMaterial = newMaterials[0];
            if (!_mergeMaterials || fallbackMaterial->getUdim().empty())
            {
                for (mx::MeshPartitionPtr geom : _geometryList)
                {
                    if (!_materialAssignments[geom])
                    {
                        assignMaterial(geom, fallbackMaterial);
                    }
                }
            }

            // Validate assigned materials.
            for (const auto& pair : _materialAssignments)
            {
                if (pair.first == getSelectedGeometry())
                {
                    mx::GlslMaterialPtr material = pair.second;
                    if (material)
                    {
                        material->bindShader();
                    }
                }
            }
        }
    }
    catch (mx::ExceptionRenderError& e)
    {
        for (const std::string& error : e.errorLog())
        {
            std::cerr << error << std::endl;
        }
    }
    catch (std::exception& e)
    {
        std::cerr << e.what() << std::endl;
    }
}

void RenderView::reloadShaders()
{
    try
    {
        for (mx::GlslMaterialPtr material : _materials)
        {
            material->generateShader(_genContext);
            for (GLenum error = glGetError(); error; error = glGetError())
            {
                std::cerr << "OpenGL error "
                          << "reload"
                          << ": " << std::to_string(error) << std::endl;
            }
        }
        return;
    }
    catch (mx::ExceptionRenderError& e)
    {
        for (const std::string& error : e.errorLog())
        {
            std::cerr << error << std::endl;
        }
    }

    _materials.clear();
}

void RenderView::initContext(mx::GenContext& context)
{
    // Initialize search path
    context.registerSourceCodeSearchPath(_searchPath);

    // Initialize color management.
    mx::DefaultColorManagementSystemPtr cms = mx::DefaultColorManagementSystem::create(context.getShaderGenerator().getTarget());
    cms->loadLibrary(_stdLib);
    context.getShaderGenerator().setColorManagementSystem(cms);

    // Initialize unit management.
    mx::UnitSystemPtr unitSystem = mx::UnitSystem::create(context.getShaderGenerator().getTarget());
    unitSystem->loadLibrary(_stdLib);
    unitSystem->setUnitConverterRegistry(_unitRegistry);
    context.getShaderGenerator().setUnitSystem(unitSystem);
    context.getOptions().targetDistanceUnit = "meter";
}

void RenderView::loadStandardLibraries()
{
    // Initialize the standard library.
    try
    {
        _stdLib = mx::createDocument();
        _xincludeFiles = mx::loadLibraries(_libraryFolders, _searchPath, _stdLib);
        if (_xincludeFiles.empty())
        {
            std::cerr << "Could not find standard data libraries on the given search path: " << _searchPath.asString() << std::endl;
        }
    }
    catch (std::exception& e)
    {
        std::cerr << "Failed to load standard data libraries: " << e.what() << std::endl;
        return;
    }

    // Initialize unit management.
    mx::UnitTypeDefPtr distanceTypeDef = _stdLib->getUnitTypeDef("distance");
    _distanceUnitConverter = mx::LinearUnitConverter::create(distanceTypeDef);
    _unitRegistry->addUnitConverter(distanceTypeDef, _distanceUnitConverter);
    mx::UnitTypeDefPtr angleTypeDef = _stdLib->getUnitTypeDef("angle");
    mx::LinearUnitConverterPtr angleConverter = mx::LinearUnitConverter::create(angleTypeDef);
    _unitRegistry->addUnitConverter(angleTypeDef, angleConverter);

    // Create the list of supported distance units.
    auto unitScales = _distanceUnitConverter->getUnitScale();
    _distanceUnitOptions.resize(unitScales.size());
    for (auto unitScale : unitScales)
    {
        int location = _distanceUnitConverter->getUnitAsInteger(unitScale.first);
        _distanceUnitOptions[location] = unitScale.first;
    }

    // Initialize the generator context.
    initContext(_genContext);
}

mx::ImagePtr RenderView::getAmbientOcclusionImage(mx::GlslMaterialPtr material)
{
    const mx::string AO_FILENAME_SUFFIX = "_ao";
    const mx::string AO_FILENAME_EXTENSION = "png";

    if (!material || !_genContext.getOptions().hwAmbientOcclusion)
    {
        return nullptr;
    }

    std::string aoSuffix = material->getUdim().empty() ? AO_FILENAME_SUFFIX : AO_FILENAME_SUFFIX + "_" + material->getUdim();
    mx::FilePath aoFilename = _meshFilename;
    aoFilename.removeExtension();
    aoFilename = aoFilename.asString() + aoSuffix;
    aoFilename.addExtension(AO_FILENAME_EXTENSION);
    return _imageHandler->acquireImage(aoFilename);
}

void RenderView::drawContents()
{
    if (_geometryList.empty() || _materials.empty())
    {
        return;
    }

    updateCameras();
    glClearColor(1.0, 1.0, 1.0, 1.0);

    // Render the current frame.
    try
    {
        renderFrame();
    }
    catch (std::exception&)
    {
        _materialAssignments.clear();
        glDisable(GL_FRAMEBUFFER_SRGB);
    }

    // Capture the current frame.
    if (_captureRequested)
    {
        _captureRequested = false;
        mx::ImagePtr frameImage = _renderFrame->getColorImage();
        if (frameImage && _imageHandler->saveImage(_captureFilename, frameImage, true))
        {
            std::cout << "Wrote frame to disk: " << _captureFilename.asString() << std::endl;
        }

        // Restore state for scene rendering.
        glViewport(0, 0, (int32_t) _screenWidth, (int32_t) _screenHeight);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glDrawBuffer(GL_BACK);
    }
}

void RenderView::applyDirectLights(mx::DocumentPtr doc)
{
    if (_lightRigDoc)
    {
        doc->importLibrary(_lightRigDoc);
        _xincludeFiles.insert(_lightRigFilename);
    }

    try
    {
        std::vector<mx::NodePtr> lights;
        _lightHandler->findLights(doc, lights);
        _lightHandler->registerLights(doc, lights, _genContext);
        _lightHandler->setLightSources(lights);
    }
    catch (std::exception& e)
    {
        std::cerr << "Failed to set up lighting" << e.what();
    }
}

void RenderView::loadEnvironmentLight()
{
    // Load the requested radiance map.
    mx::ImagePtr envRadianceMap = _imageHandler->acquireImage(_envRadianceFilename);
    if (!envRadianceMap)
    {
        return;
    }

    // Look for an irradiance map using an expected filename convention.
    mx::ImagePtr envIrradianceMap;
    mx::FilePath envIrradiancePath = _envRadianceFilename.getParentPath() / IRRADIANCE_MAP_FOLDER / _envRadianceFilename.getBaseName();
    envIrradianceMap = _imageHandler->acquireImage(envIrradiancePath);

    // If not found, then generate an irradiance map via spherical harmonics.
    if (envIrradianceMap == _imageHandler->getInvalidImage())
    {
        mx::Sh3ColorCoeffs shIrradiance = mx::projectEnvironment(envRadianceMap, true);
        envIrradianceMap = mx::renderEnvironment(shIrradiance, IRRADIANCE_MAP_WIDTH, IRRADIANCE_MAP_HEIGHT);
    }

    // Release any existing environment maps and store the new ones.
    _imageHandler->releaseRenderResources(_lightHandler->getEnvRadianceMap());
    _imageHandler->releaseRenderResources(_lightHandler->getEnvIrradianceMap());
    _lightHandler->setEnvRadianceMap(envRadianceMap);
    _lightHandler->setEnvIrradianceMap(envIrradianceMap);

    // Look for a light rig using an expected filename convention.
    _lightRigFilename = _envRadianceFilename;
    _lightRigFilename.removeExtension();
    _lightRigFilename.addExtension(mx::MTLX_EXTENSION);
    _lightRigFilename = _searchPath.find(_lightRigFilename);
    if (_lightRigFilename.exists())
    {
        _lightRigDoc = mx::createDocument();
        mx::readFromXmlFile(_lightRigDoc, _lightRigFilename, _searchPath);
    }
    else
    {
        _lightRigDoc = nullptr;
    }
}

void RenderView::renderFrame()
{
    // Initialize OpenGL state
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisable(GL_CULL_FACE);
    glDisable(GL_FRAMEBUFFER_SRGB);

    // Update lighting state.
    _lightHandler->setLightTransform(mx::Matrix44::createRotationY(_lightRotation / 180.0f * PI));

    // Update shadow state.
    mx::ShadowState shadowState;
    shadowState.ambientOcclusionGain = _ambientOcclusionGain;
    mx::NodePtr dirLight = _lightHandler->getFirstLightOfCategory(DIR_LIGHT_NODE_CATEGORY);
    if (_genContext.getOptions().hwShadowMap && dirLight)
    {
        mx::ImagePtr shadowMap = getShadowMap();
        if (shadowMap)
        {
            shadowState.shadowMap = shadowMap;
            shadowState.shadowMatrix = _viewCamera->getWorldMatrix().getInverse() *
                                       _shadowCamera->getWorldViewProjMatrix();
        }
        else
        {
            _genContext.getOptions().hwShadowMap = false;
        }
    }

    // Initialize viewport render.
    if (!_renderFrame ||
        _renderFrame->getWidth() != _screenWidth ||
        _renderFrame->getHeight() != _screenHeight)
    {
        _renderFrame = mx::GLFramebuffer::create(_screenWidth, _screenHeight, 4, mx::Image::BaseType::UINT8);
    }

    _renderFrame->bind();

    glClearColor(.70f, .70f, .75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_FRAMEBUFFER_SRGB);

    // Enable backface culling if requested.
    if (!_renderDoubleSided)
    {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    }

    // Opaque pass
    for (const auto& assignment : _materialAssignments)
    {
        mx::MeshPartitionPtr geom = assignment.first;
        mx::GlslMaterialPtr material = assignment.second;
        shadowState.ambientOcclusionMap = getAmbientOcclusionImage(material);
        if (!material)
        {
            continue;
        }

        material->bindShader();
        material->bindMesh(_geometryHandler->findParentMesh(geom));
        if (material->getProgram()->hasUniform(mx::HW::ALPHA_THRESHOLD))
        {
            material->getProgram()->bindUniform(mx::HW::ALPHA_THRESHOLD, mx::Value::createValue(0.99f));
        }
        material->bindViewInformation(_viewCamera);
        material->bindLighting(_lightHandler, _imageHandler, shadowState);
        material->bindImages(_imageHandler, _searchPath);
        material->drawPartition(geom);
        material->unbindImages(_imageHandler);
    }

    // Transparent pass
    if (_renderTransparency)
    {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        for (const auto& assignment : _materialAssignments)
        {
            mx::MeshPartitionPtr geom = assignment.first;
            mx::GlslMaterialPtr material = assignment.second;
            shadowState.ambientOcclusionMap = getAmbientOcclusionImage(material);
            if (!material || !material->hasTransparency())
            {
                continue;
            }

            material->bindShader();
            material->bindMesh(_geometryHandler->findParentMesh(geom));
            if (material->getProgram()->hasUniform(mx::HW::ALPHA_THRESHOLD))
            {
                material->getProgram()->bindUniform(mx::HW::ALPHA_THRESHOLD, mx::Value::createValue(0.001f));
            }
            material->bindViewInformation(_viewCamera);
            material->bindLighting(_lightHandler, _imageHandler, shadowState);
            material->bindImages(_imageHandler, _searchPath);
            material->drawPartition(geom);
            material->unbindImages(_imageHandler);
        }
        glDisable(GL_BLEND);
    }
    if (!_renderDoubleSided)
    {
        glDisable(GL_CULL_FACE);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Store viewport texture for render.
    _textureID = _renderFrame->getColorTexture();
}

void RenderView::initCamera()
{
    _viewCamera->setViewportSize(mx::Vector2((float) _screenWidth, (float) _screenHeight));

    // Disable user camera controls when non-centered views are requested.
    _userCameraEnabled = _cameraTarget == mx::Vector3(0.0) &&
                         _meshScale == 1.0f;

    if (!_userCameraEnabled || _geometryHandler->getMeshes().empty())
    {
        return;
    }

    const mx::Vector3& boxMax = _geometryHandler->getMaximumBounds();
    const mx::Vector3& boxMin = _geometryHandler->getMinimumBounds();
    mx::Vector3 sphereCenter = (boxMax + boxMin) * 0.5;

    mx::Matrix44 meshRotation = mx::Matrix44::createRotationZ(_meshRotation[2] / 180.0f * PI) *
                                mx::Matrix44::createRotationY(_meshRotation[1] / 180.0f * PI) *
                                mx::Matrix44::createRotationX(_meshRotation[0] / 180.0f * PI);
    _meshTranslation = -meshRotation.transformPoint(sphereCenter);
    _meshScale = IDEAL_MESH_SPHERE_RADIUS / (sphereCenter - boxMin).getMagnitude();
}

void RenderView::updateCameras()
{
    mx::Matrix44 viewMatrix, projectionMatrix;
    float aspectRatio = (float) _screenHeight / _screenHeight;
    if (_cameraViewAngle != 0.0f)
    {
        viewMatrix = mx::Camera::createViewMatrix(_cameraPosition, _cameraTarget, _cameraUp);
        float fH = std::tan(_cameraViewAngle / 360.0f * PI) * _cameraNearDist;
        float fW = fH * aspectRatio;
        projectionMatrix = mx::Camera::createPerspectiveMatrix(-fW, fW, -fH, fH, _cameraNearDist, _cameraFarDist);
    }
    else
    {
        viewMatrix = mx::Matrix44::createTranslation(mx::Vector3(0.0f, 0.0f, -ORTHO_VIEW_DISTANCE));
        float fH = ORTHO_PROJECTION_HEIGHT;
        float fW = fH * aspectRatio;
        projectionMatrix = mx::Camera::createOrthographicMatrix(-fW, fW, -fH, fH, 0.0f, ORTHO_VIEW_DISTANCE + _cameraFarDist);
    }

    mx::Matrix44 meshRotation = mx::Matrix44::createRotationZ(_meshRotation[2] / 180.0f * PI) *
                                mx::Matrix44::createRotationY(_meshRotation[1] / 180.0f * PI) *
                                mx::Matrix44::createRotationX(_meshRotation[0] / 180.0f * PI);

    mx::Matrix44 arcball = mx::Matrix44::IDENTITY;
    if (_userCameraEnabled)
    {
        arcball = _viewCamera->arcballMatrix();
    }

    _viewCamera->setWorldMatrix(meshRotation *
                                mx::Matrix44::createTranslation(_meshTranslation + _userTranslation) *
                                mx::Matrix44::createScale(mx::Vector3(_meshScale * _cameraZoom)));
    _viewCamera->setViewMatrix(arcball * viewMatrix);
    _viewCamera->setProjectionMatrix(projectionMatrix);

    _envCamera->setWorldMatrix(mx::Matrix44::createScale(mx::Vector3(300.0f)));
    _envCamera->setViewMatrix(_viewCamera->getViewMatrix());
    _envCamera->setProjectionMatrix(_viewCamera->getProjectionMatrix());

    mx::NodePtr dirLight = _lightHandler->getFirstLightOfCategory(DIR_LIGHT_NODE_CATEGORY);
    if (dirLight)
    {
        mx::Vector3 sphereCenter = (_geometryHandler->getMaximumBounds() + _geometryHandler->getMinimumBounds()) * 0.5;
        float r = (sphereCenter - _geometryHandler->getMinimumBounds()).getMagnitude();
        _shadowCamera->setWorldMatrix(meshRotation * mx::Matrix44::createTranslation(-sphereCenter));
        _shadowCamera->setProjectionMatrix(mx::Camera::createOrthographicMatrix(-r, r, -r, r, 0.0f, r * 2.0f));
        mx::ValuePtr value = dirLight->getInputValue("direction");
        if (value->isA<mx::Vector3>())
        {
            mx::Vector3 dir = mx::Matrix44::createRotationY(_lightRotation / 180.0f * PI).transformVector(value->asA<mx::Vector3>());
            _shadowCamera->setViewMatrix(mx::Camera::createViewMatrix(dir * -r, mx::Vector3(0.0f), _cameraUp));
        }
    }
}

mx::GlslMaterialPtr RenderView::getWireframeMaterial()
{
    if (!_wireMaterial)
    {
        try
        {
            mx::ShaderPtr hwShader = mx::createConstantShader(_genContext, _stdLib, "__WIRE_SHADER__", mx::Color3(1.0f));
            _wireMaterial = mx::GlslMaterial::create();
            _wireMaterial->generateShader(hwShader);
        }
        catch (std::exception& e)
        {
            std::cerr << "Failed to generate wireframe shader: " << e.what() << std::endl;
            _wireMaterial = nullptr;
        }
    }

    return _wireMaterial;
}

void RenderView::renderScreenSpaceQuad(mx::GlslMaterialPtr material)
{
    if (!_quadMesh)
        _quadMesh = mx::GeometryHandler::createQuadMesh();

    material->bindMesh(_quadMesh);
    material->drawPartition(_quadMesh->getPartition(0));
}

mx::ImagePtr RenderView::getShadowMap()
{
    if (!_shadowMap)
    {
        // Generate shaders for shadow rendering.
        if (!_shadowMaterial)
        {
            try
            {
                mx::ShaderPtr hwShader = mx::createDepthShader(_genContext, _stdLib, "__SHADOW_SHADER__");
                _shadowMaterial = mx::GlslMaterial::create();
                _shadowMaterial->generateShader(hwShader);
            }
            catch (std::exception& e)
            {
                std::cerr << "Failed to generate shadow shader: " << e.what() << std::endl;
                _shadowMaterial = nullptr;
            }
        }
        if (!_shadowBlurMaterial)
        {
            try
            {
                mx::ShaderPtr hwShader = mx::createBlurShader(_genContext, _stdLib, "__SHADOW_BLUR_SHADER__", "gaussian", 1.0f);
                _shadowBlurMaterial = mx::GlslMaterial::create();
                _shadowBlurMaterial->generateShader(hwShader);
            }
            catch (std::exception& e)
            {
                std::cerr << "Failed to generate shadow blur shader: " << e.what() << std::endl;
                _shadowBlurMaterial = nullptr;
            }
        }

        if (_shadowMaterial && _shadowBlurMaterial)
        {
            // Create framebuffer.
            mx::GLFramebufferPtr framebuffer = mx::GLFramebuffer::create(SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, 2, mx::Image::BaseType::FLOAT);
            framebuffer->bind();
            glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

            // Render shadow geometry.
            _shadowMaterial->bindShader();
            for (auto mesh : _geometryHandler->getMeshes())
            {
                _shadowMaterial->bindMesh(mesh);
                _shadowMaterial->bindViewInformation(_shadowCamera);
                for (size_t i = 0; i < mesh->getPartitionCount(); i++)
                {
                    mx::MeshPartitionPtr geom = mesh->getPartition(i);
                    _shadowMaterial->drawPartition(geom);
                }
            }
            _shadowMap = framebuffer->getColorImage();

            // Apply Gaussian blurring.
            mx::ImageSamplingProperties blurSamplingProperties;
            blurSamplingProperties.uaddressMode = mx::ImageSamplingProperties::AddressMode::CLAMP;
            blurSamplingProperties.vaddressMode = mx::ImageSamplingProperties::AddressMode::CLAMP;
            blurSamplingProperties.filterType = mx::ImageSamplingProperties::FilterType::CLOSEST;
            for (unsigned int i = 0; i < _shadowSoftness; i++)
            {
                framebuffer->bind();
                _shadowBlurMaterial->bindShader();
                if (_imageHandler->bindImage(_shadowMap, blurSamplingProperties))
                {
                    mx::GLTextureHandlerPtr textureHandler = std::static_pointer_cast<mx::GLTextureHandler>(_imageHandler);
                    int textureLocation = textureHandler->getBoundTextureLocation(_shadowMap->getResourceId());
                    if (textureLocation >= 0)
                    {
                        _shadowBlurMaterial->getProgram()->bindUniform("image_file", mx::Value::createValue(textureLocation));
                    }
                }
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
                renderScreenSpaceQuad(_shadowBlurMaterial);
                _imageHandler->releaseRenderResources(_shadowMap);
                _shadowMap = framebuffer->getColorImage();
            }

            // Restore state for scene rendering.
            glViewport(0, 0, (int32_t) _screenWidth, (int32_t) _screenHeight);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glDrawBuffer(GL_BACK);
        }
    }

    return _shadowMap;
}
