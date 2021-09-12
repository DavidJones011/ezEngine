#pragma once

#include <EditorTest/EditorTestPCH.h>

#include <EditorTest/TestClass/TestClass.h>

class ezEditorSceneDocumentTest : public ezEditorTest
{
public:
  using SUPER = ezEditorTest;

  virtual const char* GetTestName() const override;

private:
  enum SubTests
  {
    ST_Layers,
  };

  virtual void SetupSubTests() override;
  virtual ezResult InitializeTest() override;
  virtual ezResult DeInitializeTest() override;
  virtual ezTestAppRun RunSubTest(ezInt32 iIdentifier, ezUInt32 uiInvocationCount) override;

  void LayerOperations();
};
