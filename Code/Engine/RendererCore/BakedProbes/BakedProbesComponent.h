#pragma once

#include <Core/World/SettingsComponent.h>
#include <Core/World/SettingsComponentManager.h>
#include <Foundation/Types/SharedPtr.h>
#include <RendererCore/BakedProbes/BakingInterface.h>
#include <RendererCore/Declarations.h>
#include <RendererFoundation/RendererFoundationDLL.h>

struct ezMsgUpdateLocalBounds;
struct ezMsgExtractRenderData;
struct ezRenderWorldRenderEvent;
class ezAbstractObjectNode;

class EZ_RENDERERCORE_DLL ezBakedProbesComponentManager : public ezSettingsComponentManager<class ezBakedProbesComponent>
{
public:
  ezBakedProbesComponentManager(ezWorld* pWorld);
  ~ezBakedProbesComponentManager();

  virtual void Initialize() override;
  virtual void Deinitialize() override;

  ezMeshResourceHandle m_hDebugSphere;
  ezMaterialResourceHandle m_hDebugMaterial;

private:
  void RenderDebug(const ezWorldModule::UpdateContext& updateContext);
  void OnRenderEvent(const ezRenderWorldRenderEvent& e);
  void CreateDebugResources();
};

class EZ_RENDERERCORE_DLL ezBakedProbesComponent : public ezSettingsComponent
{
  EZ_DECLARE_COMPONENT_TYPE(ezBakedProbesComponent, ezSettingsComponent, ezBakedProbesComponentManager);

public:
  ezBakedProbesComponent();
  ~ezBakedProbesComponent();

  virtual void OnActivated() override;
  virtual void OnDeactivated() override;

  ezBakingSettings m_Settings; // [ property ]

  void SetShowDebugOverlay(bool bShow);                            // [ property ]
  bool GetShowDebugOverlay() const { return m_bShowDebugOverlay; } // [ property ]

  void SetShowDebugProbes(bool bShow);                           // [ property ]
  bool GetShowDebugProbes() const { return m_bShowDebugProbes; } // [ property ]

  void SetUseTestPosition(bool bUse);                            // [ property ]
  bool GetUseTestPosition() const { return m_bUseTestPosition; } // [ property ]

  void SetTestPosition(const ezVec3& pos);                         // [ property ]
  const ezVec3& GetTestPosition() const { return m_TestPosition; } // [ property ]

  void OnUpdateLocalBounds(ezMsgUpdateLocalBounds& msg);
  void OnExtractRenderData(ezMsgExtractRenderData& msg) const;

  virtual void SerializeComponent(ezWorldWriter& stream) const override;
  virtual void DeserializeComponent(ezWorldReader& stream) override;

private:
  void RenderDebugOverlay();
  void OnObjectCreated(const ezAbstractObjectNode& node);

  ezHashedString m_sProbeTreeResourcePrefix;

  bool m_bShowDebugOverlay = false;
  bool m_bShowDebugProbes = false;
  bool m_bUseTestPosition = false;
  ezVec3 m_TestPosition = ezVec3::ZeroVector();

  struct RenderDebugViewTask;
  ezSharedPtr<RenderDebugViewTask> m_pRenderDebugViewTask;

  ezGALTextureHandle m_hDebugViewTexture;
};