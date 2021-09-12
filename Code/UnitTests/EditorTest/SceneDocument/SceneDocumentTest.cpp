#include <EditorTest/EditorTestPCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorFramework/Assets/AssetDocument.h>
#include <EditorTest/SceneDocument/SceneDocumentTest.h>
#include <Foundation/IO/OSFile.h>
#include <ToolsFoundation/Object/ObjectAccessorBase.h>
#include <EditorPluginScene/Scene/Scene2Document.h>

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
  ezScene2Document* pDoc = nullptr;
  ezStringBuilder sName;
  EZ_TEST_BLOCK(ezTestBlock::Enabled, "Create Document")
  {
    sName = m_sProjectPath;
    sName.AppendPath("LayerOperations.ezScene");
    pDoc = static_cast<ezScene2Document*>(m_pApplication->m_pEditorApp->CreateDocument(sName, ezDocumentFlags::RequestWindow));
    EZ_TEST_BOOL(pDoc != nullptr);
    ProcessEvents();
  }
  //ProcessEvents(999999999);
  ezUuid layer1Guid;
  EZ_TEST_STATUS(pDoc->CreateLayer("Layer1", layer1Guid));
  //ProcessEvents(999999999);
  EZ_TEST_STATUS(pDoc->SetActiveLayer(layer1Guid));
  EZ_TEST_BOOL(pDoc->GetActiveLayer() == layer1Guid);
  EZ_TEST_BOOL(pDoc->IsLayerVisible(layer1Guid));
  EZ_TEST_BOOL(pDoc->IsLayerLoaded(layer1Guid));
  pDoc->GetLayerDocument(layer1Guid);

  EZ_TEST_BLOCK(ezTestBlock::Enabled, "Save Document")
  {
    // Save doc twice in a row without processing messages and then close it.
    ezDocumentObject* pMeshAsset = pDoc->GetObjectManager()->GetRootObject()->GetChildren()[0];
    ezObjectAccessorBase* pAcc = pDoc->GetObjectAccessor();
    ezInt32 iOrder = 0;
    ezTaskGroupID id = pDoc->SaveDocumentAsync(
      [&iOrder](ezDocument* doc, ezStatus res) {
        EZ_TEST_INT(iOrder, 0);
        iOrder = 1;
      },
      true);
  }
  
  pDoc->GetDocumentManager()->CloseDocument(pDoc);
}
