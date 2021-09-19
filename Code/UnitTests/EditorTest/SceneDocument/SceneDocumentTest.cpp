#include <EditorTest/EditorTestPCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorFramework/Assets/AssetDocument.h>
#include <EditorPluginScene/Scene/LayerDocument.h>
#include <EditorPluginScene/Scene/Scene2Document.h>
#include <EditorTest/SceneDocument/SceneDocumentTest.h>
#include <Foundation/IO/OSFile.h>
#include <ToolsFoundation/Object/ObjectAccessorBase.h>

static ezEditorSceneDocumentTest s_EditorSceneDocumentTest;

const char* ezEditorSceneDocumentTest::GetTestName() const
{
  return "Scene Document Tests";
}

void ezEditorSceneDocumentTest::SetupSubTests()
{
  AddSubTest("Layers", SubTests::ST_Layers);
}

ezResult ezEditorSceneDocumentTest::InitializeTest()
{
  if (SUPER::InitializeTest().Failed())
    return EZ_FAILURE;

  if (SUPER::CreateAndLoadProject("SceneTestProject").Failed())
    return EZ_FAILURE;

  return EZ_SUCCESS;
}

ezResult ezEditorSceneDocumentTest::DeInitializeTest()
{
  if (SUPER::DeInitializeTest().Failed())
    return EZ_FAILURE;

  return EZ_SUCCESS;
}

ezTestAppRun ezEditorSceneDocumentTest::RunSubTest(ezInt32 iIdentifier, ezUInt32 uiInvocationCount)
{
  switch (iIdentifier)
  {
    case SubTests::ST_Layers:
      LayerOperations();
      break;
  }
  return ezTestAppRun::Quit;
}

void ezEditorSceneDocumentTest::LayerOperations()
{
  ezStringBuilder sName;
  sName = m_sProjectPath;
  sName.AppendPath("LayerOperations.ezScene");

  ezScene2Document* pDoc = nullptr;
  ezEventSubscriptionID layerEventsID = 0;
  ezHybridArray<ezScene2LayerEvent, 2> expectedEvents;
  ezUuid sceneGuid;
  ezUuid layer1Guid;
  ezLayerDocument* pLayer1 = nullptr;

  auto TestLayerEvents = [&expectedEvents](const ezScene2LayerEvent& e) {
    if (EZ_TEST_BOOL(!expectedEvents.IsEmpty()))
    {
      // If we pass in an invalid guid it's considered fine as we might not know the ID, e.g. when creating a layer.
      EZ_TEST_BOOL(!expectedEvents[0].m_layerGuid.IsValid() || expectedEvents[0].m_layerGuid == e.m_layerGuid);
      EZ_TEST_BOOL(expectedEvents[0].m_Type == e.m_Type);
      expectedEvents.RemoveAtAndCopy(0);
    }
  };

  EZ_TEST_BLOCK(ezTestBlock::Enabled, "Create Document")
  {
    pDoc = static_cast<ezScene2Document*>(m_pApplication->m_pEditorApp->CreateDocument(sName, ezDocumentFlags::RequestWindow));
    if (!EZ_TEST_BOOL(pDoc != nullptr))
      return;

    sceneGuid = pDoc->GetGuid();
    layerEventsID = pDoc->m_LayerEvents.AddEventHandler(TestLayerEvents);
    ProcessEvents();
  }

  EZ_TEST_BLOCK(ezTestBlock::Enabled, "Create Layer")
  {
    EZ_TEST_BOOL(pDoc->GetActiveLayer() == sceneGuid);
    EZ_TEST_BOOL(pDoc->IsLayerVisible(sceneGuid));
    EZ_TEST_BOOL(pDoc->IsLayerLoaded(sceneGuid));

    expectedEvents.PushBack({ezScene2LayerEvent::Type::LayerAdded, ezUuid()});
    expectedEvents.PushBack({ezScene2LayerEvent::Type::LayerLoaded, ezUuid()});
    EZ_TEST_STATUS(pDoc->CreateLayer("Layer1", layer1Guid));

    expectedEvents.PushBack({ezScene2LayerEvent::Type::ActiveLayerChanged, layer1Guid});
    EZ_TEST_STATUS(pDoc->SetActiveLayer(layer1Guid));
    EZ_TEST_BOOL(pDoc->GetActiveLayer() == layer1Guid);
    EZ_TEST_BOOL(pDoc->IsLayerVisible(layer1Guid));
    EZ_TEST_BOOL(pDoc->IsLayerLoaded(layer1Guid));
    pLayer1 = ezDynamicCast<ezLayerDocument*>(pDoc->GetLayerDocument(layer1Guid));
    EZ_TEST_BOOL(pLayer1 != nullptr);

    ezHybridArray<ezSceneDocument*, 2> layers;
    pDoc->GetLoadedLayers(layers);
    EZ_TEST_INT(layers.GetCount(), 2);
    EZ_TEST_BOOL(layers.Contains(pLayer1));
    EZ_TEST_BOOL(layers.Contains(pDoc));
  }

  EZ_TEST_BLOCK(ezTestBlock::Enabled, "Undo/Redo Layer Creation")
  {
    // Undo / redo
    expectedEvents.PushBack({ezScene2LayerEvent::Type::ActiveLayerChanged, sceneGuid});
    EZ_TEST_STATUS(pDoc->SetActiveLayer(sceneGuid));
    // Initial scene setup exists in the scene undo stack
    EZ_TEST_INT(pDoc->GetCommandHistory()->GetUndoStackSize(), 2);
    EZ_TEST_INT(pDoc->GetSceneCommandHistory()->GetUndoStackSize(), 2);
    expectedEvents.PushBack({ezScene2LayerEvent::Type::LayerUnloaded, layer1Guid});
    expectedEvents.PushBack({ezScene2LayerEvent::Type::LayerRemoved, layer1Guid});
    EZ_TEST_STATUS(pDoc->GetCommandHistory()->Undo(1));
    expectedEvents.PushBack({ezScene2LayerEvent::Type::LayerAdded, layer1Guid});
    expectedEvents.PushBack({ezScene2LayerEvent::Type::LayerLoaded, layer1Guid});
    EZ_TEST_STATUS(pDoc->GetCommandHistory()->Redo(1));

    pLayer1 = ezDynamicCast<ezLayerDocument*>(pDoc->GetLayerDocument(layer1Guid));
    EZ_TEST_BOOL(pLayer1 != nullptr);
  }

  EZ_TEST_BLOCK(ezTestBlock::Enabled, "Save and Close Document")
  {
    bool bSaved = false;
    ezTaskGroupID id = pDoc->SaveDocumentAsync(
      [&bSaved](ezDocument* doc, ezStatus res) {
        bSaved = true;
      },
      true);

    pDoc->m_LayerEvents.RemoveEventHandler(layerEventsID);
    pDoc->GetDocumentManager()->CloseDocument(pDoc);
    EZ_TEST_BOOL(ezTaskSystem::IsTaskGroupFinished(id));
    EZ_TEST_BOOL(bSaved);
  }

  EZ_TEST_BLOCK(ezTestBlock::Enabled, "Reload Document")
  {
    pDoc = static_cast<ezScene2Document*>(m_pApplication->m_pEditorApp->OpenDocument(sName, ezDocumentFlags::RequestWindow));
    if (!EZ_TEST_BOOL(pDoc != nullptr))
      return;
    layerEventsID = pDoc->m_LayerEvents.AddEventHandler(TestLayerEvents);
    ProcessEvents();

    EZ_TEST_BOOL(pDoc->GetActiveLayer() == sceneGuid);
    EZ_TEST_BOOL(pDoc->IsLayerVisible(sceneGuid));
    EZ_TEST_BOOL(pDoc->IsLayerLoaded(sceneGuid));

    pLayer1 = ezDynamicCast<ezLayerDocument*>(pDoc->GetLayerDocument(layer1Guid));
    ezHybridArray<ezSceneDocument*, 2> layers;
    pDoc->GetLoadedLayers(layers);
    EZ_TEST_INT(layers.GetCount(), 2);
    EZ_TEST_BOOL(layers.Contains(pLayer1));
    EZ_TEST_BOOL(layers.Contains(pDoc));
  }

  EZ_TEST_BLOCK(ezTestBlock::Enabled, "Toggle Layer Visibility")
  {
    EZ_TEST_BOOL(pDoc->GetActiveLayer() == sceneGuid);
    expectedEvents.PushBack({ezScene2LayerEvent::Type::ActiveLayerChanged, layer1Guid});
    EZ_TEST_STATUS(pDoc->SetActiveLayer(layer1Guid));
    EZ_TEST_BOOL(pDoc->GetActiveLayer() == layer1Guid);
    EZ_TEST_BOOL(pDoc->IsLayerVisible(layer1Guid));
    EZ_TEST_BOOL(pDoc->IsLayerLoaded(layer1Guid));

    expectedEvents.PushBack({ezScene2LayerEvent::Type::LayerInvisible, layer1Guid});
    EZ_TEST_STATUS(pDoc->SetLayerVisible(layer1Guid, false));
    EZ_TEST_BOOL(!pDoc->IsLayerVisible(layer1Guid));
    ProcessEvents();
    expectedEvents.PushBack({ezScene2LayerEvent::Type::LayerVisible, layer1Guid});
    EZ_TEST_STATUS(pDoc->SetLayerVisible(layer1Guid, true));
    EZ_TEST_BOOL(pDoc->IsLayerVisible(layer1Guid));
    ProcessEvents();
  }

  EZ_TEST_BLOCK(ezTestBlock::Enabled, "Toggle Layer Loaded")
  {
    expectedEvents.PushBack({ezScene2LayerEvent::Type::LayerInvisible, layer1Guid});
    EZ_TEST_STATUS(pDoc->SetLayerVisible(layer1Guid, false));

    expectedEvents.PushBack({ezScene2LayerEvent::Type::ActiveLayerChanged, sceneGuid});
    expectedEvents.PushBack({ezScene2LayerEvent::Type::LayerUnloaded, layer1Guid});
    EZ_TEST_STATUS(pDoc->SetLayerLoaded(layer1Guid, false));

    expectedEvents.PushBack({ezScene2LayerEvent::Type::LayerLoaded, layer1Guid});
    EZ_TEST_STATUS(pDoc->SetLayerLoaded(layer1Guid, true));
  }

  EZ_TEST_BLOCK(ezTestBlock::Enabled, "Delete Layer")
  {
    expectedEvents.PushBack({ezScene2LayerEvent::Type::ActiveLayerChanged, layer1Guid});
    EZ_TEST_STATUS(pDoc->SetActiveLayer(layer1Guid));

    expectedEvents.PushBack({ezScene2LayerEvent::Type::ActiveLayerChanged, sceneGuid});
    expectedEvents.PushBack({ezScene2LayerEvent::Type::LayerUnloaded, layer1Guid});
    expectedEvents.PushBack({ezScene2LayerEvent::Type::LayerRemoved, layer1Guid});
    EZ_TEST_STATUS(pDoc->DeleteLayer(layer1Guid));
  }

  EZ_TEST_BLOCK(ezTestBlock::Enabled, "Undo/Redo Layer Deletion")
  {
    EZ_TEST_INT(pDoc->GetCommandHistory()->GetUndoStackSize(), 1);
    EZ_TEST_INT(pDoc->GetSceneCommandHistory()->GetUndoStackSize(), 1);
    expectedEvents.PushBack({ezScene2LayerEvent::Type::LayerAdded, layer1Guid});
    expectedEvents.PushBack({ezScene2LayerEvent::Type::LayerLoaded, layer1Guid});
    EZ_TEST_STATUS(pDoc->GetCommandHistory()->Undo(1));
    EZ_TEST_BOOL(pDoc->IsLayerVisible(layer1Guid));
    EZ_TEST_BOOL(pDoc->IsLayerLoaded(layer1Guid));

    expectedEvents.PushBack({ezScene2LayerEvent::Type::LayerUnloaded, layer1Guid});
    expectedEvents.PushBack({ezScene2LayerEvent::Type::LayerRemoved, layer1Guid});
    EZ_TEST_STATUS(pDoc->GetCommandHistory()->Redo(1));
  }

  EZ_TEST_BLOCK(ezTestBlock::Enabled, "Close Document")
  {
    bool bSaved = false;
    ezTaskGroupID id = pDoc->SaveDocumentAsync(
      [&bSaved](ezDocument* doc, ezStatus res) {
        bSaved = true;
      },
      true);

    pDoc->m_LayerEvents.RemoveEventHandler(layerEventsID);
    pDoc->GetDocumentManager()->CloseDocument(pDoc);
    EZ_TEST_BOOL(ezTaskSystem::IsTaskGroupFinished(id));
    EZ_TEST_BOOL(bSaved);
  }
  ProcessEvents(999999999);
  return;
}
