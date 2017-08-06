﻿#include <PCH.h>
#include <GameEngine/MixedReality/MixedRealityFramework.h>
#include <GameEngine/GameApplication/GameApplication.h>
#include <RendererCore/Pipeline/Declarations.h>

//////////////////////////////////////////////////////////////////////////

#ifdef BUILDSYSTEM_ENABLE_MIXEDREALITY_SUPPORT

#include <WindowsMixedReality/HolographicSpace.h>
#include <WindowsMixedReality/Graphics/MixedRealityDX11Device.h>
#include <WindowsMixedReality/SpatialMapping/SurfaceReconstructionMeshManager.h>
#include <RendererFoundation/Resources/RenderTargetSetup.h>
#include <RendererCore/RenderWorld/RenderWorld.h>
#include <RendererCore/Pipeline/View.h>
#include <WindowsMixedReality/Graphics/MixedRealityCamera.h>

EZ_IMPLEMENT_SINGLETON(ezMixedRealityFramework);

ezMixedRealityFramework::ezMixedRealityFramework(ezCamera* pCameraForSynchronization)
  : m_SingletonRegistrar(this)
{
  Startup(pCameraForSynchronization);
}

ezMixedRealityFramework::~ezMixedRealityFramework()
{
  Shutdown();
}

void ezMixedRealityFramework::Startup(ezCamera* pCameraForSynchronization)
{
  ezGameApplication::SetOverrideDefaultDeviceCreator([this](const ezGALDeviceCreationDescription& desc) -> ezGALDevice*
  {
    auto pHoloSpace = ezWindowsHolographicSpace::GetSingleton();
    if (pHoloSpace->IsAvailable())
    {
      ezGALDevice* pDevice = EZ_DEFAULT_NEW(ezGALMixedRealityDeviceDX11, desc);
      OnDeviceCreated(true);
      return pDevice;
    }
    else
    {
      ezGALDevice* pDevice = EZ_DEFAULT_NEW(ezGALDeviceDX11, desc);
      OnDeviceCreated(false);
      return pDevice;
    }
  });

  auto pHolospace = ezWindowsHolographicSpace::GetSingleton();

  if (pHolospace == nullptr)
  {
    m_pHolospaceToDestroy = EZ_DEFAULT_NEW(ezWindowsHolographicSpace);
    pHolospace = m_pHolospaceToDestroy.Borrow();
    pHolospace->InitForMainCoreWindow();
  }

  if (pHolospace->GetDefaultReferenceFrame() == nullptr)
  {
    pHolospace->CreateDefaultReferenceFrame();
  }

  SetCameraForPredictionSynchronization(pCameraForSynchronization);
}

void ezMixedRealityFramework::Shutdown()
{
  ezGameApplication::GetGameApplicationInstance()->m_Events.RemoveEventHandler(ezMakeDelegate(&ezMixedRealityFramework::GameApplicationEventHandler, this));

  m_pSpatialMappingManager = nullptr;
  m_pHolospaceToDestroy = nullptr;
  m_pCameraToSynchronize = nullptr;
}

void ezMixedRealityFramework::GameApplicationEventHandler(const ezGameApplicationEvent& e)
{
  if (e.m_Type == ezGameApplicationEvent::Type::AfterWorldUpdates)
  {
    if (m_pCameraToSynchronize)
    {
      ezWindowsHolographicSpace::GetSingleton()->SynchronizeCameraPrediction(*m_pCameraToSynchronize);
    }
  }

  if (e.m_Type == ezGameApplicationEvent::Type::BeginAppTick)
  {
    ezWindowsHolographicSpace::GetSingleton()->ProcessAddedRemovedCameras();
  }
}

void ezMixedRealityFramework::OnDeviceCreated(bool bHolographicDevice)
{
  if (bHolographicDevice)
  {
    ezGameApplication::GetGameApplicationInstance()->m_Events.AddEventHandler(ezMakeDelegate(&ezMixedRealityFramework::GameApplicationEventHandler, this));

    m_pSpatialMappingManager = EZ_DEFAULT_NEW(ezSurfaceReconstructionMeshManager);
  }
}

void ezMixedRealityFramework::SetCameraForPredictionSynchronization(ezCamera* pCamera)
{
  if (m_pCameraToSynchronize == pCamera)
    return;

  EZ_ASSERT_DEV(pCamera == nullptr || pCamera->GetCameraMode() == ezCameraMode::Stereo, "Incorrect camera mode. Should be 'Stereo'.");
  m_pCameraToSynchronize = pCamera;
}

ezViewHandle ezMixedRealityFramework::CreateHolographicView(ezWindowBase* pWindow, const ezRenderPipelineResourceHandle& hRenderPipeline, ezCamera* pCamera, ezWorld* pWorld /*= nullptr*/)
{
  auto pHoloSpace = ezWindowsHolographicSpace::GetSingleton();

  if (!pHoloSpace->IsAvailable())
  {
    ezLog::Error("ezMixedRealityFramework::CreateMainView: Holographic space is not available.");
    return ezViewHandle();
  }

  while (pHoloSpace->GetCameras().IsEmpty())
  {
    if (!ezGameApplication::GetGameApplicationInstance()->ProcessWindowMessages())
    {
      EZ_REPORT_FAILURE("No window has been added to ezGameApplication, thus no Holo CameraAdded events can arrive!");
      return ezViewHandle();
    }

    pHoloSpace->ProcessAddedRemovedCameras();
  }

  pCamera->SetCameraMode(ezCameraMode::Stereo, 90.0f, 0.1f, 100.0f);
  SetCameraForPredictionSynchronization(pCamera);

  auto hRemoteWindowSwapChain = ezGALDevice::GetDefaultDevice()->GetPrimarySwapChain();
  EZ_ASSERT_DEBUG(!hRemoteWindowSwapChain.IsInvalidated(), "Primary swap chain is still invalid after a holographic camera has been added.");

  const ezGALSwapChain* pSwapChain = ezGALDevice::GetDefaultDevice()->GetSwapChain(hRemoteWindowSwapChain);
  auto hBackBufferRTV = ezGALDevice::GetDefaultDevice()->GetDefaultRenderTargetView(pSwapChain->GetBackBufferTexture());

  ezView* pMainView = nullptr;
  ezViewHandle hMainView = ezRenderWorld::CreateView("Holographic View", pMainView);
  pMainView->SetCameraUsageHint(ezCameraUsageHint::MainView);

  ezGALRenderTagetSetup renderTargetSetup;
  renderTargetSetup.SetRenderTarget(0, hBackBufferRTV);
  pMainView->SetRenderTargetSetup(renderTargetSetup);
  pMainView->SetRenderPipelineResource(hRenderPipeline);
  pMainView->SetCamera(pCamera);
  pMainView->SetViewport(pHoloSpace->GetCameras()[0]->GetViewport());
  pMainView->SetWorld(pWorld);

  ezRenderWorld::AddMainView(hMainView);

  ezGameApplication::GetGameApplicationInstance()->AddWindow(pWindow, hRemoteWindowSwapChain);

  return hMainView;
}

#endif