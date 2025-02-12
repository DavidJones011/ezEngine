#include <EditorPluginAssets/EditorPluginAssetsPCH.h>

#include <Core/World/World.h>
#include <EditorPluginVisualScript/VisualScriptGraph/VisualScriptCompiler.h>
#include <EditorPluginVisualScript/VisualScriptGraph/VisualScriptTypeDeduction.h>
#include <Foundation/IO/ChunkStream.h>
#include <Foundation/IO/StringDeduplicationContext.h>
#include <Foundation/SimdMath/SimdRandom.h>
#include <Foundation/Utilities/DGMLWriter.h>

namespace
{
  ezResult ExtractPropertyName(ezStringView sPinName, ezStringView& out_sPropertyName, ezUInt32* out_pArrayIndex = nullptr)
  {
    const char* szBracket = sPinName.FindSubString("[");
    if (szBracket == nullptr)
      return EZ_FAILURE;

    out_sPropertyName = ezStringView(sPinName.GetStartPointer(), szBracket);

    if (out_pArrayIndex != nullptr)
    {
      return ezConversionUtils::StringToUInt(szBracket + 1, *out_pArrayIndex);
    }

    return EZ_SUCCESS;
  }

  void MakeSubfunctionName(const ezDocumentObject* pObject, const ezDocumentObject* pEntryObject, ezStringBuilder& out_sName)
  {
    ezVariant sNameProperty = pObject->GetTypeAccessor().GetValue("Name");
    ezUInt32 uiHash = ezHashHelper<ezUuid>::Hash(pObject->GetGuid());

    out_sName.Format("{}_{}_{}", pEntryObject != nullptr ? ezVisualScriptNodeManager::GetNiceFunctionName(pEntryObject) : "", sNameProperty, ezArgU(uiHash, 8, true, 16));
  }

  ezVisualScriptDataType::Enum FinalizeDataType(ezVisualScriptDataType::Enum dataType)
  {
    ezVisualScriptDataType::Enum result = dataType;
    if (result == ezVisualScriptDataType::EnumValue)
      result = ezVisualScriptDataType::Int64;

    return result;
  }

  using FillUserDataFunction = ezResult (*)(ezVisualScriptCompiler::AstNode& inout_astNode, ezVisualScriptCompiler* pCompiler, const ezDocumentObject* pObject, const ezDocumentObject* pEntryObject);

  static ezResult FillUserData_CoroutineMode(ezVisualScriptCompiler::AstNode& inout_astNode, ezVisualScriptCompiler* pCompiler, const ezDocumentObject* pObject, const ezDocumentObject* pEntryObject)
  {
    inout_astNode.m_Value = pObject->GetTypeAccessor().GetValue("CoroutineMode");
    return EZ_SUCCESS;
  }

  static ezResult FillUserData_ReflectedPropertyOrFunction(ezVisualScriptCompiler::AstNode& inout_astNode, ezVisualScriptCompiler* pCompiler, const ezDocumentObject* pObject, const ezDocumentObject* pEntryObject)
  {
    auto pNodeDesc = ezVisualScriptNodeRegistry::GetSingleton()->GetNodeDescForType(pObject->GetType());
    if (pNodeDesc->m_pTargetType != nullptr)
      inout_astNode.m_sTargetTypeName.Assign(pNodeDesc->m_pTargetType->GetTypeName());

    ezVariantArray propertyNames;
    for (auto& pProp : pNodeDesc->m_TargetProperties)
    {
      ezHashedString sPropertyName;
      sPropertyName.Assign(pProp->GetPropertyName());
      propertyNames.PushBack(sPropertyName);
    }

    inout_astNode.m_Value = propertyNames;

    return EZ_SUCCESS;
  }

  static ezResult FillUserData_DynamicReflectedProperty(ezVisualScriptCompiler::AstNode& inout_astNode, ezVisualScriptCompiler* pCompiler, const ezDocumentObject* pObject, const ezDocumentObject* pEntryObject)
  {
    auto pTargetType = ezVisualScriptTypeDeduction::GetReflectedType(pObject);
    auto pTargetProperty = ezVisualScriptTypeDeduction::GetReflectedProperty(pObject);
    if (pTargetType == nullptr || pTargetProperty == nullptr)
      return EZ_FAILURE;

    inout_astNode.m_sTargetTypeName.Assign(pTargetType->GetTypeName());

    ezVariantArray propertyNames;
    {
      ezHashedString sPropertyName;
      sPropertyName.Assign(pTargetProperty->GetPropertyName());
      propertyNames.PushBack(sPropertyName);
    }

    inout_astNode.m_Value = propertyNames;

    return EZ_SUCCESS;
  }

  static ezResult FillUserData_ConstantValue(ezVisualScriptCompiler::AstNode& inout_astNode, ezVisualScriptCompiler* pCompiler, const ezDocumentObject* pObject, const ezDocumentObject* pEntryObject)
  {
    inout_astNode.m_Value = pObject->GetTypeAccessor().GetValue("Value");
    inout_astNode.m_DeductedDataType = ezVisualScriptDataType::FromVariantType(inout_astNode.m_Value.GetType());
    return EZ_SUCCESS;
  }

  static ezResult FillUserData_VariableName(ezVisualScriptCompiler::AstNode& inout_astNode, ezVisualScriptCompiler* pCompiler, const ezDocumentObject* pObject, const ezDocumentObject* pEntryObject)
  {
    inout_astNode.m_Value = pObject->GetTypeAccessor().GetValue("Name");

    ezStringView sName = inout_astNode.m_Value.Get<ezString>().GetView();

    ezVariant defaultValue;
    if (static_cast<const ezVisualScriptNodeManager*>(pObject->GetDocumentObjectManager())->GetVariableDefaultValue(sName, defaultValue).Failed())
    {
      ezLog::Error("Invalid variable named '{}'", sName);
      return EZ_FAILURE;
    }

    return EZ_SUCCESS;
  }

  static ezResult FillUserData_Switch(ezVisualScriptCompiler::AstNode& inout_astNode, ezVisualScriptCompiler* pCompiler, const ezDocumentObject* pObject, const ezDocumentObject* pEntryObject)
  {
    inout_astNode.m_DeductedDataType = ezVisualScriptDataType::Int64;

    ezVariantArray casesVarArray;

    auto pNodeDesc = ezVisualScriptNodeRegistry::GetSingleton()->GetNodeDescForType(pObject->GetType());
    if (pNodeDesc->m_pTargetType != nullptr)
    {
      ezHybridArray<ezReflectionUtils::EnumKeyValuePair, 16> enumKeysAndValues;
      ezReflectionUtils::GetEnumKeysAndValues(pNodeDesc->m_pTargetType, enumKeysAndValues, ezReflectionUtils::EnumConversionMode::ValueNameOnly);
      for (auto& keyAndValue : enumKeysAndValues)
      {
        casesVarArray.PushBack(keyAndValue.m_iValue);
      }
    }
    else
    {
      ezVariant casesVar = pObject->GetTypeAccessor().GetValue("Cases");
      casesVarArray = casesVar.Get<ezVariantArray>();
      for (auto& caseVar : casesVarArray)
      {
        if (caseVar.IsA<ezString>())
        {
          inout_astNode.m_DeductedDataType = ezVisualScriptDataType::HashedString;
          caseVar = ezTempHashedString(caseVar.Get<ezString>()).GetHash();
        }
        else if (caseVar.IsA<ezHashedString>())
        {
          inout_astNode.m_DeductedDataType = ezVisualScriptDataType::HashedString;
          caseVar = caseVar.Get<ezHashedString>().GetHash();
        }
      }
    }

    inout_astNode.m_Value = casesVarArray;
    return EZ_SUCCESS;
  }

  static ezResult FillUserData_Builtin_Compare(ezVisualScriptCompiler::AstNode& inout_astNode, ezVisualScriptCompiler* pCompiler, const ezDocumentObject* pObject, const ezDocumentObject* pEntryObject)
  {
    inout_astNode.m_Value = pObject->GetTypeAccessor().GetValue("Operator");
    return EZ_SUCCESS;
  }

  static ezResult FillUserData_Builtin_TryGetComponentOfBaseType(ezVisualScriptCompiler::AstNode& inout_astNode, ezVisualScriptCompiler* pCompiler, const ezDocumentObject* pObject, const ezDocumentObject* pEntryObject)
  {
    auto typeName = pObject->GetTypeAccessor().GetValue("TypeName");
    const ezRTTI* pType = ezRTTI::FindTypeByName(typeName.Get<ezString>());
    if (pType == nullptr)
    {
      ezLog::Error("Invalid type '{}' for GameObject::TryGetComponentOfBaseType node.", typeName);
      return EZ_FAILURE;
    }

    inout_astNode.m_sTargetTypeName.Assign(pType->GetTypeName());
    return EZ_SUCCESS;
  }

  static ezResult FillUserData_Builtin_StartCoroutine(ezVisualScriptCompiler::AstNode& inout_astNode, ezVisualScriptCompiler* pCompiler, const ezDocumentObject* pObject, const ezDocumentObject* pEntryObject)
  {
    EZ_SUCCEED_OR_RETURN(FillUserData_CoroutineMode(inout_astNode, pCompiler, pObject, pEntryObject));

    auto pManager = static_cast<const ezVisualScriptNodeManager*>(pObject->GetDocumentObjectManager());
    ezHybridArray<const ezVisualScriptPin*, 16> pins;
    pManager->GetOutputExecutionPins(pObject, pins);

    const ezUInt32 uiCoroutineBodyIndex = 1;
    auto connections = pManager->GetConnections(*pins[uiCoroutineBodyIndex]);
    if (connections.IsEmpty() == false)
    {
      ezStringBuilder sFunctionName;
      MakeSubfunctionName(pObject, pEntryObject, sFunctionName);

      ezStringBuilder sFullName;
      sFullName.Set(pCompiler->GetCompiledModule().m_sScriptClassName, "::", sFunctionName, "<Coroutine>");

      inout_astNode.m_sTargetTypeName.Assign(sFullName);

      return pCompiler->AddFunction(sFunctionName, connections[0]->GetTargetPin().GetParent(), pObject);
    }

    return EZ_SUCCESS;
  }

  static FillUserDataFunction s_TypeToFillUserDataFunctions[] = {
    nullptr,                                   // Invalid,
    &FillUserData_CoroutineMode,               // EntryCall,
    &FillUserData_CoroutineMode,               // EntryCall_Coroutine,
    &FillUserData_ReflectedPropertyOrFunction, // MessageHandler,
    &FillUserData_ReflectedPropertyOrFunction, // MessageHandler_Coroutine,
    &FillUserData_ReflectedPropertyOrFunction, // ReflectedFunction,
    &FillUserData_DynamicReflectedProperty,    // GetReflectedProperty,
    &FillUserData_DynamicReflectedProperty,    // SetReflectedProperty,
    &FillUserData_ReflectedPropertyOrFunction, // InplaceCoroutine,
    nullptr,                                   // GetOwner,
    &FillUserData_ReflectedPropertyOrFunction, // SendMessage,

    nullptr, // FirstBuiltin,

    &FillUserData_ConstantValue, // Builtin_Constant,
    &FillUserData_VariableName,  // Builtin_GetVariable,
    &FillUserData_VariableName,  // Builtin_SetVariable,
    &FillUserData_VariableName,  // Builtin_IncVariable,
    &FillUserData_VariableName,  // Builtin_DecVariable,

    nullptr,              // Builtin_Branch,
    &FillUserData_Switch, // Builtin_Switch,
    nullptr,              // Builtin_Loop,

    nullptr,                       // Builtin_And,
    nullptr,                       // Builtin_Or,
    nullptr,                       // Builtin_Not,
    &FillUserData_Builtin_Compare, // Builtin_Compare,
    nullptr,                       // Builtin_IsValid,
    nullptr,                       // Builtin_Select,

    nullptr, // Builtin_Add,
    nullptr, // Builtin_Subtract,
    nullptr, // Builtin_Multiply,
    nullptr, // Builtin_Divide,

    nullptr, // Builtin_ToBool,
    nullptr, // Builtin_ToByte,
    nullptr, // Builtin_ToInt,
    nullptr, // Builtin_ToInt64,
    nullptr, // Builtin_ToFloat,
    nullptr, // Builtin_ToDouble,
    nullptr, // Builtin_ToString,
    nullptr, // Builtin_String_Format,
    nullptr, // Builtin_ToHashedString,
    nullptr, // Builtin_ToVariant,
    nullptr, // Builtin_Variant_ConvertTo,

    nullptr, // Builtin_MakeArray

    &FillUserData_Builtin_TryGetComponentOfBaseType, // Builtin_TryGetComponentOfBaseType

    &FillUserData_Builtin_StartCoroutine, // Builtin_StartCoroutine,
    nullptr,                              // Builtin_StopCoroutine,
    nullptr,                              // Builtin_StopAllCoroutines,
    nullptr,                              // Builtin_WaitForAll,
    nullptr,                              // Builtin_WaitForAny,
    nullptr,                              // Builtin_Yield,

    nullptr, // LastBuiltin,
  };

  static_assert(EZ_ARRAY_SIZE(s_TypeToFillUserDataFunctions) == ezVisualScriptNodeDescription::Type::Count);

  ezResult FillUserData(ezVisualScriptCompiler::AstNode& inout_astNode, ezVisualScriptCompiler* pCompiler, const ezDocumentObject* pObject, const ezDocumentObject* pEntryObject)
  {
    if (pObject == nullptr)
      return EZ_SUCCESS;

    auto nodeType = inout_astNode.m_Type;
    EZ_ASSERT_DEBUG(nodeType >= 0 && nodeType < EZ_ARRAY_SIZE(s_TypeToFillUserDataFunctions), "Out of bounds access");
    auto func = s_TypeToFillUserDataFunctions[nodeType];

    if (func != nullptr)
    {
      EZ_SUCCEED_OR_RETURN(func(inout_astNode, pCompiler, pObject, pEntryObject));
    }

    return EZ_SUCCESS;
  }

} // namespace

//////////////////////////////////////////////////////////////////////////

ezVisualScriptCompiler::CompiledModule::CompiledModule()
  : m_ConstantDataStorage(ezSharedPtr<ezVisualScriptDataDescription>(&m_ConstantDataDesc, nullptr))
{
  // Prevent the data desc from being deleted by fake shared ptr above
  m_ConstantDataDesc.AddRef();
}

ezResult ezVisualScriptCompiler::CompiledModule::Serialize(ezStreamWriter& inout_stream) const
{
  EZ_ASSERT_DEV(m_sScriptClassName.IsEmpty() == false, "Invalid script class name");

  ezStringDeduplicationWriteContext stringDedup(inout_stream);

  ezChunkStreamWriter chunk(stringDedup.Begin());
  chunk.BeginStream(1);

  {
    chunk.BeginChunk("Header", 1);
    chunk << m_sBaseClassName;
    chunk << m_sScriptClassName;
    chunk.EndChunk();
  }

  {
    chunk.BeginChunk("FunctionGraphs", 1);
    chunk << m_Functions.GetCount();

    for (auto& function : m_Functions)
    {
      chunk << function.m_sName;
      chunk << function.m_Type;
      chunk << function.m_CoroutineCreationMode;

      EZ_SUCCEED_OR_RETURN(ezVisualScriptGraphDescription::Serialize(function.m_NodeDescriptions, function.m_LocalDataDesc, chunk));
    }

    chunk.EndChunk();
  }

  {
    chunk.BeginChunk("ConstantData", 1);
    EZ_SUCCEED_OR_RETURN(m_ConstantDataDesc.Serialize(chunk));
    EZ_SUCCEED_OR_RETURN(m_ConstantDataStorage.Serialize(chunk));
    chunk.EndChunk();
  }

  {
    chunk.BeginChunk("InstanceData", 1);
    EZ_SUCCEED_OR_RETURN(m_InstanceDataDesc.Serialize(chunk));
    EZ_SUCCEED_OR_RETURN(chunk.WriteHashTable(m_InstanceDataMapping.m_Content));
    chunk.EndChunk();
  }

  chunk.EndStream();

  return stringDedup.End();
}

//////////////////////////////////////////////////////////////////////////

// static
ezUInt32 ezVisualScriptCompiler::ConnectionHasher::Hash(const Connection& c)
{
  ezUInt32 uiHashes[] = {
    ezHashHelper<void*>::Hash(c.m_pPrev),
    ezHashHelper<void*>::Hash(c.m_pCurrent),
    ezHashHelper<ezUInt32>::Hash(c.m_Type),
    ezHashHelper<ezUInt32>::Hash(c.m_uiPrevPinIndex),
  };
  return ezHashingUtils::xxHash32(uiHashes, sizeof(uiHashes));
}

// static
bool ezVisualScriptCompiler::ConnectionHasher::Equal(const Connection& a, const Connection& b)
{
  return a.m_pPrev == b.m_pPrev &&
         a.m_pCurrent == b.m_pCurrent &&
         a.m_Type == b.m_Type &&
         a.m_uiPrevPinIndex == b.m_uiPrevPinIndex;
}

//////////////////////////////////////////////////////////////////////////

ezVisualScriptCompiler::ezVisualScriptCompiler() = default;
ezVisualScriptCompiler::~ezVisualScriptCompiler() = default;

void ezVisualScriptCompiler::InitModule(ezStringView sBaseClassName, ezStringView sScriptClassName)
{
  m_Module.m_sBaseClassName = sBaseClassName;
  m_Module.m_sScriptClassName = sScriptClassName;
}

ezResult ezVisualScriptCompiler::AddFunction(ezStringView sName, const ezDocumentObject* pEntryObject, const ezDocumentObject* pParentObject)
{
  if (m_pManager == nullptr)
  {
    m_pManager = static_cast<const ezVisualScriptNodeManager*>(pEntryObject->GetDocumentObjectManager());
  }
  EZ_ASSERT_DEV(m_pManager == pEntryObject->GetDocumentObjectManager(), "Can't add functions from different document");

  for (auto& existingFunction : m_Module.m_Functions)
  {
    if (existingFunction.m_sName == sName)
    {
      ezLog::Error("A function named '{}' already exists. Function names need to unique.", sName);
      return EZ_FAILURE;
    }
  }

  AstNode* pEntryAstNode = BuildAST(pEntryObject);
  if (pEntryAstNode == nullptr)
    return EZ_FAILURE;

  auto& function = m_Module.m_Functions.ExpandAndGetRef();
  function.m_sName = sName;
  function.m_Type = pEntryAstNode->m_Type;

  {
    auto pObjectWithCoroutineMode = pParentObject != nullptr ? pParentObject : pEntryObject;
    auto mode = pObjectWithCoroutineMode->GetTypeAccessor().GetValue("CoroutineMode");
    if (mode.IsA<ezInt64>())
    {
      function.m_CoroutineCreationMode = static_cast<ezScriptCoroutineCreationMode::Enum>(mode.Get<ezInt64>());
    }
    else
    {
      function.m_CoroutineCreationMode = ezScriptCoroutineCreationMode::AllowOverlap;
    }
  }

  m_EntryAstNodes.PushBack(pEntryAstNode);
  EZ_ASSERT_DEBUG(m_Module.m_Functions.GetCount() == m_EntryAstNodes.GetCount(), "");

  return EZ_SUCCESS;
}

ezResult ezVisualScriptCompiler::Compile(ezStringView sDebugAstOutputPath)
{
  for (ezUInt32 i = 0; i < m_Module.m_Functions.GetCount(); ++i)
  {
    auto& function = m_Module.m_Functions[i];
    AstNode* pEntryAstNode = m_EntryAstNodes[i];

    DumpAST(pEntryAstNode, sDebugAstOutputPath, function.m_sName, "_00");

    EZ_SUCCEED_OR_RETURN(InlineConstants(pEntryAstNode));
    EZ_SUCCEED_OR_RETURN(InsertTypeConversions(pEntryAstNode));
    EZ_SUCCEED_OR_RETURN(InlineVariables(pEntryAstNode));

    DumpAST(pEntryAstNode, sDebugAstOutputPath, function.m_sName, "_01_TypeConv");

    EZ_SUCCEED_OR_RETURN(BuildDataExecutions(pEntryAstNode));

    DumpAST(pEntryAstNode, sDebugAstOutputPath, function.m_sName, "_02_FlattenedExec");

    EZ_SUCCEED_OR_RETURN(FillDataOutputConnections(pEntryAstNode));
    EZ_SUCCEED_OR_RETURN(AssignLocalVariables(pEntryAstNode, function.m_LocalDataDesc));
    EZ_SUCCEED_OR_RETURN(BuildNodeDescriptions(pEntryAstNode, function.m_NodeDescriptions));

    DumpGraph(function.m_NodeDescriptions, sDebugAstOutputPath, function.m_sName, "_Graph");

    m_PinIdToDataDesc.Clear();
  }

  EZ_SUCCEED_OR_RETURN(FinalizeDataOffsets());
  EZ_SUCCEED_OR_RETURN(FinalizeConstantData());

  return EZ_SUCCESS;
}

ezUInt32 ezVisualScriptCompiler::GetPinId(const ezVisualScriptPin* pPin)
{
  ezUInt32 uiId = 0;
  if (pPin != nullptr && m_PinToId.TryGetValue(pPin, uiId))
    return uiId;

  uiId = m_uiNextPinId++;
  if (pPin != nullptr)
  {
    m_PinToId.Insert(pPin, uiId);
  }
  return uiId;
}

ezVisualScriptCompiler::DataOutput& ezVisualScriptCompiler::GetDataOutput(const DataInput& dataInput)
{
  if (dataInput.m_uiSourcePinIndex < dataInput.m_pSourceNode->m_Outputs.GetCount())
  {
    return dataInput.m_pSourceNode->m_Outputs[dataInput.m_uiSourcePinIndex];
  }

  EZ_ASSERT_DEBUG(false, "This code should be never reached");
  static DataOutput dummy;
  return dummy;
}

ezVisualScriptCompiler::DefaultInput ezVisualScriptCompiler::GetDefaultPointerInput(const ezRTTI* pDataType)
{
  DefaultInput defaultInput;
  if (m_DefaultInputs.TryGetValue(pDataType, defaultInput) == false)
  {
    if (pDataType == ezGetStaticRTTI<ezGameObject>() || pDataType == ezGetStaticRTTI<ezGameObjectHandle>())
    {
      auto pGetOwnerNode = &m_AstNodes.ExpandAndGetRef();
      pGetOwnerNode->m_Type = ezVisualScriptNodeDescription::Type::GetScriptOwner;
      pGetOwnerNode->m_bImplicitExecution = true;

      {
        auto& dataOutput = pGetOwnerNode->m_Outputs.ExpandAndGetRef();
        dataOutput.m_uiId = GetPinId(nullptr);
        dataOutput.m_DataType = ezVisualScriptDataType::TypedPointer;
      }

      {
        auto& dataOutput = pGetOwnerNode->m_Outputs.ExpandAndGetRef();
        dataOutput.m_uiId = GetPinId(nullptr);
        dataOutput.m_DataType = ezVisualScriptDataType::GameObject;
      }

      defaultInput.m_pSourceNode = pGetOwnerNode;
      defaultInput.m_uiSourcePinIndex = 1;
      m_DefaultInputs.Insert(pDataType, defaultInput);
    }
    else if (pDataType == ezGetStaticRTTI<ezWorld>())
    {
      auto pGetOwnerNode = &m_AstNodes.ExpandAndGetRef();
      pGetOwnerNode->m_Type = ezVisualScriptNodeDescription::Type::GetScriptOwner;
      pGetOwnerNode->m_bImplicitExecution = true;

      auto& dataOutput = pGetOwnerNode->m_Outputs.ExpandAndGetRef();
      dataOutput.m_uiId = GetPinId(nullptr);
      dataOutput.m_DataType = ezVisualScriptDataType::TypedPointer;

      defaultInput.m_pSourceNode = pGetOwnerNode;
      defaultInput.m_uiSourcePinIndex = 0;
      m_DefaultInputs.Insert(pDataType, defaultInput);
    }
  }

  return defaultInput;
}

ezVisualScriptCompiler::DataOffset ezVisualScriptCompiler::GetInstanceDataOffset(ezHashedString sName, ezVisualScriptDataType::Enum dataType)
{
  ezVisualScriptInstanceData instanceData;
  if (m_Module.m_InstanceDataMapping.m_Content.TryGetValue(sName, instanceData) == false)
  {
    EZ_ASSERT_DEBUG(dataType < ezVisualScriptDataType::Count, "Invalid data type");
    auto& offsetAndCount = m_Module.m_InstanceDataDesc.m_PerTypeInfo[dataType];
    instanceData.m_DataOffset.m_uiByteOffset = offsetAndCount.m_uiCount;
    instanceData.m_DataOffset.m_uiType = dataType;
    instanceData.m_DataOffset.m_uiSource = DataOffset::Source::Instance;
    ++offsetAndCount.m_uiCount;

    m_pManager->GetVariableDefaultValue(sName, instanceData.m_DefaultValue).AssertSuccess();

    m_Module.m_InstanceDataMapping.m_Content.Insert(sName, instanceData);
  }

  return instanceData.m_DataOffset;
}

ezVisualScriptCompiler::AstNode* ezVisualScriptCompiler::BuildAST(const ezDocumentObject* pEntryObject)
{
  ezHashTable<const ezDocumentObject*, AstNode*> objectToAstNode;
  ezHybridArray<const ezVisualScriptPin*, 16> pins;

  auto CreateAstNode = [&](const ezDocumentObject* pObject) -> AstNode* {
    auto pNodeDesc = ezVisualScriptNodeRegistry::GetSingleton()->GetNodeDescForType(pObject->GetType());
    EZ_ASSERT_DEV(pNodeDesc != nullptr, "Invalid node type");

    auto& astNode = m_AstNodes.ExpandAndGetRef();
    astNode.m_Type = pNodeDesc->m_Type;
    astNode.m_DeductedDataType = FinalizeDataType(GetDeductedType(pObject));
    astNode.m_bImplicitExecution = pNodeDesc->m_bImplicitExecution;

    if (FillUserData(astNode, this, pObject, pEntryObject).Failed())
      return nullptr;

    objectToAstNode.Insert(pObject, &astNode);

    return &astNode;
  };

  AstNode* pEntryAstNode = CreateAstNode(pEntryObject);
  if (pEntryAstNode == nullptr)
    return nullptr;

  if (ezVisualScriptNodeDescription::Type::IsEntry(pEntryAstNode->m_Type) == false)
  {
    auto& astNode = m_AstNodes.ExpandAndGetRef();
    astNode.m_Type = ezVisualScriptNodeDescription::Type::EntryCall;
    astNode.m_Next.PushBack(pEntryAstNode);

    pEntryAstNode = &astNode;
  }

  ezHybridArray<const ezDocumentObject*, 64> nodeStack;
  nodeStack.PushBack(pEntryObject);

  while (nodeStack.IsEmpty() == false)
  {
    const ezDocumentObject* pObject = nodeStack.PeekBack();
    nodeStack.PopBack();

    AstNode* pAstNode = nullptr;
    EZ_VERIFY(objectToAstNode.TryGetValue(pObject, pAstNode), "Implementation error");

    if (ezVisualScriptNodeDescription::Type::MakesOuterCoroutine(pAstNode->m_Type))
    {
      MarkAsCoroutine(pEntryAstNode);
    }

    m_pManager->GetInputDataPins(pObject, pins);
    ezUInt32 uiNextInputPinIndex = 0;

    auto pNodeDesc = ezVisualScriptNodeRegistry::GetSingleton()->GetNodeDescForType(pObject->GetType());
    for (auto& pinDesc : pNodeDesc->m_InputPins)
    {
      if (pinDesc.IsExecutionPin())
        continue;

      AstNode* pAstNodeToAddInput = pAstNode;
      bool bArrayInput = false;
      if (pinDesc.m_sDynamicPinProperty.IsEmpty() == false)
      {
        const ezAbstractProperty* pProp = pObject->GetType()->FindPropertyByName(pinDesc.m_sDynamicPinProperty);
        if (pProp == nullptr)
          return nullptr;

        if (pProp->GetCategory() == ezPropertyCategory::Array)
        {
          auto pMakeArrayAstNode = &m_AstNodes.ExpandAndGetRef();
          pMakeArrayAstNode->m_Type = ezVisualScriptNodeDescription::Type::Builtin_MakeArray;
          pMakeArrayAstNode->m_bImplicitExecution = true;

          auto& dataOutput = pMakeArrayAstNode->m_Outputs.ExpandAndGetRef();
          dataOutput.m_uiId = GetPinId(nullptr);
          dataOutput.m_DataType = ezVisualScriptDataType::Array;

          auto& dataInput = pAstNode->m_Inputs.ExpandAndGetRef();
          dataInput.m_pSourceNode = pMakeArrayAstNode;
          dataInput.m_uiId = GetPinId(nullptr);
          dataInput.m_uiSourcePinIndex = 0;
          dataInput.m_DataType = ezVisualScriptDataType::Array;

          pAstNodeToAddInput = pMakeArrayAstNode;
          bArrayInput = true;
        }
      }

      while (uiNextInputPinIndex < pins.GetCount())
      {
        auto pPin = pins[uiNextInputPinIndex];

        ezStringView sPropertyName = pPin->GetName();
        ezUInt32 uiArrayIndex = 0;
        ExtractPropertyName(sPropertyName, sPropertyName, &uiArrayIndex).IgnoreResult();

        if (pinDesc.m_sName.GetView() != sPropertyName)
          break;

        auto connections = m_pManager->GetConnections(*pPin);
        if (pPin->IsRequired() && connections.IsEmpty())
        {
          ezLog::Error("Required input '{}' for '{}' is not connected", pPin->GetName(), GetNiceTypeName(pObject));
          return nullptr;
        }

        ezVisualScriptDataType::Enum targetDataType = pPin->GetResolvedScriptDataType();
        if (targetDataType == ezVisualScriptDataType::Invalid)
        {
          ezLog::Error("Can't deduct type for pin '{}.{}'. The pin is not connected or all node properties are invalid.", GetNiceTypeName(pObject), pPin->GetName());
          return nullptr;
        }

        auto& dataInput = pAstNodeToAddInput->m_Inputs.ExpandAndGetRef();
        dataInput.m_uiId = GetPinId(pPin);
        dataInput.m_DataType = bArrayInput ? ezVisualScriptDataType::Variant : FinalizeDataType(targetDataType);

        if (connections.IsEmpty())
        {
          if (ezVisualScriptDataType::IsPointer(dataInput.m_DataType))
          {
            auto defaultInput = GetDefaultPointerInput(pPin->GetDataType());
            if (defaultInput.m_pSourceNode != nullptr)
            {
              dataInput.m_pSourceNode = defaultInput.m_pSourceNode;
              dataInput.m_uiSourcePinIndex = defaultInput.m_uiSourcePinIndex;
            }
          }
          else
          {
            ezStringBuilder sTmp;
            const char* szPropertyName = sPropertyName.GetData(sTmp);

            ezVariant value = pObject->GetTypeAccessor().GetValue(szPropertyName);
            if (value.IsValid() && pPin->HasDynamicPinProperty())
            {
              EZ_ASSERT_DEBUG(value.IsA<ezVariantArray>(), "Implementation error");
              value = value.Get<ezVariantArray>()[uiArrayIndex];
            }

            ezVisualScriptDataType::Enum valueDataType = ezVisualScriptDataType::FromVariantType(value.GetType());
            if (dataInput.m_DataType != ezVisualScriptDataType::Variant)
            {
              value = value.ConvertTo(ezVisualScriptDataType::GetVariantType(dataInput.m_DataType));
              if (value.IsValid() == false)
              {
                ezLog::Error("Failed to convert '{}.{}' of type '{}' to '{}'.", GetNiceTypeName(pObject), pPin->GetName(), ezVisualScriptDataType::GetName(valueDataType), ezVisualScriptDataType::GetName(dataInput.m_DataType));
                return nullptr;
              }

              valueDataType = dataInput.m_DataType;
            }

            auto& constantNode = m_AstNodes.ExpandAndGetRef();
            constantNode.m_Type = ezVisualScriptNodeDescription::Type::Builtin_Constant;
            constantNode.m_DeductedDataType = valueDataType;
            constantNode.m_bImplicitExecution = true;
            constantNode.m_Value = value;

            auto& dataOutput = constantNode.m_Outputs.ExpandAndGetRef();
            dataOutput.m_uiId = GetPinId(nullptr);
            dataOutput.m_DataType = valueDataType;

            dataInput.m_pSourceNode = &constantNode;
            dataInput.m_uiSourcePinIndex = 0;
          }
        }
        else
        {
          auto& sourcePin = static_cast<const ezVisualScriptPin&>(connections[0]->GetSourcePin());
          const ezDocumentObject* pSourceObject = sourcePin.GetParent();

          AstNode* pSourceAstNode;
          if (objectToAstNode.TryGetValue(pSourceObject, pSourceAstNode) == false)
          {
            pSourceAstNode = CreateAstNode(pSourceObject);
            if (pSourceAstNode == nullptr)
              return nullptr;

            nodeStack.PushBack(pSourceObject);
          }

          ezVisualScriptDataType::Enum sourceDataType = sourcePin.GetResolvedScriptDataType();
          if (sourceDataType == ezVisualScriptDataType::Invalid)
          {
            ezLog::Error("Can't deduct type for pin '{}.{}'. The pin is not connected or all node properties are invalid.", GetNiceTypeName(pSourceObject), sourcePin.GetName());
            return nullptr;
          }

          if (sourcePin.CanConvertTo(*pPin) == false)
          {
            ezLog::Error("Can't implicitly convert pin '{}.{}' of type '{}' connected to pin '{}.{}' of type '{}'", GetNiceTypeName(pSourceObject), sourcePin.GetName(), sourcePin.GetDataTypeName(), GetNiceTypeName(pObject), pPin->GetName(), pPin->GetDataTypeName());
            return nullptr;
          }

          dataInput.m_pSourceNode = pSourceAstNode;
          dataInput.m_uiSourcePinIndex = sourcePin.GetDataPinIndex();
        }

        ++uiNextInputPinIndex;
      }
    }

    m_pManager->GetOutputDataPins(pObject, pins);
    for (auto pPin : pins)
    {
      auto& dataOutput = pAstNode->m_Outputs.ExpandAndGetRef();
      dataOutput.m_uiId = GetPinId(pPin);
      dataOutput.m_DataType = FinalizeDataType(pPin->GetResolvedScriptDataType());
    }

    m_pManager->GetOutputExecutionPins(pObject, pins);
    for (auto pPin : pins)
    {
      auto connections = m_pManager->GetConnections(*pPin);
      if (connections.IsEmpty() || pPin->SplitExecution())
      {
        pAstNode->m_Next.PushBack(nullptr);
        continue;
      }

      EZ_ASSERT_DEV(connections.GetCount() == 1, "Output execution pins should only have one connection");
      const ezDocumentObject* pNextNode = connections[0]->GetTargetPin().GetParent();

      AstNode* pNextAstNode;
      if (objectToAstNode.TryGetValue(pNextNode, pNextAstNode) == false)
      {
        pNextAstNode = CreateAstNode(pNextNode);
        if (pNextAstNode == nullptr)
          return nullptr;

        nodeStack.PushBack(pNextNode);
      }

      pAstNode->m_Next.PushBack(pNextAstNode);
    }
  }

  return pEntryAstNode;
}

void ezVisualScriptCompiler::MarkAsCoroutine(AstNode* pEntryAstNode)
{
  switch (pEntryAstNode->m_Type)
  {
    case ezVisualScriptNodeDescription::Type::EntryCall:
      pEntryAstNode->m_Type = ezVisualScriptNodeDescription::Type::EntryCall_Coroutine;
      break;
    case ezVisualScriptNodeDescription::Type::MessageHandler:
      pEntryAstNode->m_Type = ezVisualScriptNodeDescription::Type::MessageHandler_Coroutine;
      break;
    case ezVisualScriptNodeDescription::Type::EntryCall_Coroutine:
    case ezVisualScriptNodeDescription::Type::MessageHandler_Coroutine:
      // Already a coroutine
      break;
      EZ_DEFAULT_CASE_NOT_IMPLEMENTED;
  }
}

ezResult ezVisualScriptCompiler::InsertTypeConversions(AstNode* pEntryAstNode)
{
  ezHashSet<const AstNode*> nodesWithInsertedMakeArrayNode;

  return TraverseAst(pEntryAstNode, ConnectionType::All,
    [&](const Connection& connection) {
      if (connection.m_Type == ConnectionType::Data)
      {
        auto& dataInput = connection.m_pPrev->m_Inputs[connection.m_uiPrevPinIndex];
        auto& dataOutput = GetDataOutput(dataInput);

        if (dataOutput.m_DataType != dataInput.m_DataType)
        {
          auto nodeType = ezVisualScriptNodeDescription::Type::GetConversionType(dataInput.m_DataType);

          auto& astNode = m_AstNodes.ExpandAndGetRef();
          astNode.m_Type = nodeType;
          astNode.m_DeductedDataType = dataOutput.m_DataType;
          astNode.m_bImplicitExecution = true;

          auto& newDataInput = astNode.m_Inputs.ExpandAndGetRef();
          newDataInput.m_pSourceNode = dataInput.m_pSourceNode;
          newDataInput.m_uiId = GetPinId(nullptr);
          newDataInput.m_uiSourcePinIndex = dataInput.m_uiSourcePinIndex;
          newDataInput.m_DataType = dataOutput.m_DataType;

          auto& newDataOutput = astNode.m_Outputs.ExpandAndGetRef();
          newDataOutput.m_uiId = GetPinId(nullptr);
          newDataOutput.m_DataType = dataInput.m_DataType;

          dataInput.m_pSourceNode = &astNode;
          dataInput.m_uiSourcePinIndex = 0;
        }
      }

      return VisitorResult::Continue;
    });
}

ezResult ezVisualScriptCompiler::InlineConstants(AstNode* pEntryAstNode)
{
  return TraverseAst(pEntryAstNode, ConnectionType::All,
    [&](const Connection& connection) {
      auto pCurrentNode = connection.m_pCurrent;
      for (auto& dataInput : pCurrentNode->m_Inputs)
      {
        if (m_PinIdToDataDesc.Contains(dataInput.m_uiId))
          continue;

        auto pSourceNode = dataInput.m_pSourceNode;
        if (pSourceNode == nullptr)
          continue;

        if (pSourceNode->m_Type == ezVisualScriptNodeDescription::Type::Builtin_Constant)
        {
          auto dataType = pSourceNode->m_DeductedDataType;

          ezUInt32 uiIndex = ezInvalidIndex;
          if (m_ConstantDataToIndex.TryGetValue(pSourceNode->m_Value, uiIndex) == false)
          {
            auto& offsetAndCount = m_Module.m_ConstantDataDesc.m_PerTypeInfo[dataType];
            uiIndex = offsetAndCount.m_uiCount;
            ++offsetAndCount.m_uiCount;

            m_ConstantDataToIndex.Insert(pSourceNode->m_Value, uiIndex);
          }

          DataDesc dataDesc;
          dataDesc.m_DataOffset = DataOffset(uiIndex, dataType, DataOffset::Source::Constant);
          m_PinIdToDataDesc.Insert(dataInput.m_uiId, dataDesc);

          dataInput.m_pSourceNode = nullptr;
          dataInput.m_uiSourcePinIndex = 0;
        }
      }

      return VisitorResult::Continue;
    });
}

ezResult ezVisualScriptCompiler::InlineVariables(AstNode* pEntryAstNode)
{
  return TraverseAst(pEntryAstNode, ConnectionType::All,
    [&](const Connection& connection) {
      auto pCurrentNode = connection.m_pCurrent;
      for (auto& dataInput : pCurrentNode->m_Inputs)
      {
        if (m_PinIdToDataDesc.Contains(dataInput.m_uiId))
          continue;

        auto pSourceNode = dataInput.m_pSourceNode;
        if (pSourceNode == nullptr)
          continue;

        if (pSourceNode->m_Type == ezVisualScriptNodeDescription::Type::Builtin_GetVariable)
        {
          auto& dataOutput = pSourceNode->m_Outputs[0];

          ezHashedString sName;
          sName.Assign(pSourceNode->m_Value.Get<ezString>());

          DataDesc dataDesc;
          dataDesc.m_DataOffset = GetInstanceDataOffset(sName, dataOutput.m_DataType);
          m_PinIdToDataDesc.Insert(dataInput.m_uiId, dataDesc);

          dataInput.m_pSourceNode = nullptr;
          dataInput.m_uiSourcePinIndex = 0;
        }
      }

      if (pCurrentNode->m_Type == ezVisualScriptNodeDescription::Type::Builtin_SetVariable ||
          pCurrentNode->m_Type == ezVisualScriptNodeDescription::Type::Builtin_IncVariable ||
          pCurrentNode->m_Type == ezVisualScriptNodeDescription::Type::Builtin_DecVariable)
      {
        ezHashedString sName;
        sName.Assign(pCurrentNode->m_Value.Get<ezString>());

        DataDesc dataDesc;
        dataDesc.m_DataOffset = GetInstanceDataOffset(sName, pCurrentNode->m_DeductedDataType);

        if (pCurrentNode->m_Type != ezVisualScriptNodeDescription::Type::Builtin_SetVariable)
        {
          if (pCurrentNode->m_Inputs.IsEmpty())
          {
            auto& dataInput = pCurrentNode->m_Inputs.ExpandAndGetRef();
            dataInput.m_uiId = GetPinId(nullptr);
            dataInput.m_uiSourcePinIndex = 0;
            dataInput.m_DataType = pCurrentNode->m_DeductedDataType;
          }

          m_PinIdToDataDesc.Insert(pCurrentNode->m_Inputs[0].m_uiId, dataDesc);
        }

        {
          if (pCurrentNode->m_Outputs.IsEmpty())
          {
            auto& dataOutput = pCurrentNode->m_Outputs.ExpandAndGetRef();
            dataOutput.m_uiId = GetPinId(nullptr);
            dataOutput.m_DataType = pCurrentNode->m_DeductedDataType;
          }

          m_PinIdToDataDesc.Insert(pCurrentNode->m_Outputs[0].m_uiId, dataDesc);
        }
      }

      return VisitorResult::Continue;
    });
}

ezResult ezVisualScriptCompiler::BuildDataStack(AstNode* pEntryAstNode, ezDynamicArray<AstNode*>& out_Stack)
{
  ezHashSet<const AstNode*> visitedNodes;
  out_Stack.Clear();

  EZ_SUCCEED_OR_RETURN(TraverseAst(pEntryAstNode, ConnectionType::Data,
    [&](const Connection& connection) {
      if (visitedNodes.Insert(connection.m_pCurrent))
        return VisitorResult::Stop;

      if (connection.m_pCurrent->m_bImplicitExecution == false)
        return VisitorResult::Stop;

      out_Stack.PushBack(connection.m_pCurrent);

      return VisitorResult::Continue;
    }));

  // Make unique
  ezHashTable<AstNode*, AstNode*> oldToNewNodes;
  for (ezUInt32 i = out_Stack.GetCount(); i > 0; --i)
  {
    auto& pDataNode = out_Stack[i - 1];

    if (pDataNode->m_Next.IsEmpty())
    {
      // remap inputs to new nodes
      for (auto& dataInput : pDataNode->m_Inputs)
      {
        AstNode* pNewNode = nullptr;
        if (oldToNewNodes.TryGetValue(dataInput.m_pSourceNode, pNewNode))
        {
          dataInput.m_pSourceNode = pNewNode;
        }
      }
    }
    else
    {
      auto& newDataNode = m_AstNodes.ExpandAndGetRef();
      newDataNode.m_Type = pDataNode->m_Type;
      newDataNode.m_DeductedDataType = pDataNode->m_DeductedDataType;
      newDataNode.m_bImplicitExecution = pDataNode->m_bImplicitExecution;
      newDataNode.m_sTargetTypeName = pDataNode->m_sTargetTypeName;
      newDataNode.m_Value = pDataNode->m_Value;

      for (auto& dataInput : pDataNode->m_Inputs)
      {
        auto& newDataInput = newDataNode.m_Inputs.ExpandAndGetRef();
        if (dataInput.m_pSourceNode != nullptr)
        {
          EZ_VERIFY(oldToNewNodes.TryGetValue(dataInput.m_pSourceNode, newDataInput.m_pSourceNode), "");
        }
        newDataInput.m_uiId = GetPinId(nullptr);
        newDataInput.m_uiSourcePinIndex = dataInput.m_uiSourcePinIndex;
        newDataInput.m_DataType = dataInput.m_DataType;

        DataDesc dataDesc;
        if (m_PinIdToDataDesc.TryGetValue(dataInput.m_uiId, dataDesc))
        {
          m_PinIdToDataDesc.Insert(newDataInput.m_uiId, dataDesc);
        }
      }

      for (auto& dataOutput : pDataNode->m_Outputs)
      {
        auto& newDataOutput = newDataNode.m_Outputs.ExpandAndGetRef();
        newDataOutput.m_uiId = GetPinId(nullptr);
        newDataOutput.m_DataType = dataOutput.m_DataType;

        DataDesc dataDesc;
        if (m_PinIdToDataDesc.TryGetValue(dataOutput.m_uiId, dataDesc))
        {
          m_PinIdToDataDesc.Insert(newDataOutput.m_uiId, dataDesc);
        }
      }

      oldToNewNodes.Insert(pDataNode, &newDataNode);
      pDataNode = &newDataNode;
    }
  }

  // Connect next execution
  if (out_Stack.GetCount() > 1)
  {
    AstNode* pLastDataNode = out_Stack.PeekBack();
    for (ezUInt32 i = out_Stack.GetCount() - 1; i > 0; --i)
    {
      auto& pDataNode = out_Stack[i - 1];
      pLastDataNode->m_Next.PushBack(pDataNode);
      pLastDataNode = pDataNode;
    }
  }

  // Remap inputs
  for (auto& dataInput : pEntryAstNode->m_Inputs)
  {
    if (dataInput.m_pSourceNode != nullptr)
    {
      oldToNewNodes.TryGetValue(dataInput.m_pSourceNode, dataInput.m_pSourceNode);
    }
  }

  return EZ_SUCCESS;
}

ezResult ezVisualScriptCompiler::BuildDataExecutions(AstNode* pEntryAstNode)
{
  ezHybridArray<Connection, 64> allExecConnections;

  EZ_SUCCEED_OR_RETURN(TraverseAst(pEntryAstNode, ConnectionType::Execution,
    [&](const Connection& connection) {
      allExecConnections.PushBack(connection);
      return VisitorResult::Continue;
    }));

  ezHybridArray<AstNode*, 64> nodeStack;
  ezHashTable<AstNode*, AstNode*> nodeToFirstDataNode;

  for (const auto& connection : allExecConnections)
  {
    AstNode* pFirstDataNode = nullptr;
    if (nodeToFirstDataNode.TryGetValue(connection.m_pCurrent, pFirstDataNode) == false)
    {
      if (BuildDataStack(connection.m_pCurrent, nodeStack).Failed())
        return EZ_FAILURE;

      if (nodeStack.IsEmpty() == false)
      {
        pFirstDataNode = nodeStack.PeekBack();

        AstNode* pLastDataNode = nodeStack[0];
        pLastDataNode->m_Next.PushBack(connection.m_pCurrent);
      }
    }

    if (pFirstDataNode != nullptr)
    {
      connection.m_pPrev->m_Next[connection.m_uiPrevPinIndex] = pFirstDataNode;
    }
    nodeToFirstDataNode.Insert(connection.m_pCurrent, pFirstDataNode);
  }

  return EZ_SUCCESS;
}

ezResult ezVisualScriptCompiler::FillDataOutputConnections(AstNode* pEntryAstNode)
{
  return TraverseAst(pEntryAstNode, ConnectionType::All,
    [&](const Connection& connection) {
      if (connection.m_Type == ConnectionType::Data)
      {
        auto& dataInput = connection.m_pPrev->m_Inputs[connection.m_uiPrevPinIndex];
        auto& dataOutput = GetDataOutput(dataInput);

        EZ_ASSERT_DEBUG(dataInput.m_pSourceNode == connection.m_pCurrent, "");
        if (dataOutput.m_TargetNodes.Contains(connection.m_pPrev) == false)
        {
          dataOutput.m_TargetNodes.PushBack(connection.m_pPrev);
        }
      }

      return VisitorResult::Continue;
    });
}

ezResult ezVisualScriptCompiler::AssignLocalVariables(AstNode* pEntryAstNode, ezVisualScriptDataDescription& inout_localDataDesc)
{
  ezDynamicArray<DataOffset> freeDataOffsets;

  return TraverseAst(pEntryAstNode, ConnectionType::Execution,
    [&](const Connection& connection) {
      // Outputs first so we don't end up using the same data as input and output
      for (auto& dataOutput : connection.m_pCurrent->m_Outputs)
      {
        if (m_PinIdToDataDesc.Contains(dataOutput.m_uiId))
          continue;

        if (dataOutput.m_TargetNodes.IsEmpty() == false)
        {
          DataOffset dataOffset;
          dataOffset.m_uiType = dataOutput.m_DataType;

          for (ezUInt32 i = 0; i < freeDataOffsets.GetCount(); ++i)
          {
            auto freeDataOffset = freeDataOffsets[i];
            if (freeDataOffset.m_uiType == dataOffset.m_uiType)
            {
              dataOffset = freeDataOffset;
              freeDataOffsets.RemoveAtAndSwap(i);
              break;
            }
          }

          if (dataOffset.IsValid() == false)
          {
            EZ_ASSERT_DEBUG(dataOffset.GetType() < ezVisualScriptDataType::Count, "Invalid data type");
            auto& offsetAndCount = inout_localDataDesc.m_PerTypeInfo[dataOffset.m_uiType];
            dataOffset.m_uiByteOffset = offsetAndCount.m_uiCount;
            ++offsetAndCount.m_uiCount;
          }

          DataDesc dataDesc;
          dataDesc.m_DataOffset = dataOffset;
          dataDesc.m_uiUsageCounter = dataOutput.m_TargetNodes.GetCount();
          m_PinIdToDataDesc.Insert(dataOutput.m_uiId, dataDesc);
        }
      }

      for (auto& dataInput : connection.m_pCurrent->m_Inputs)
      {
        if (m_PinIdToDataDesc.Contains(dataInput.m_uiId) || dataInput.m_pSourceNode == nullptr)
          continue;

        DataDesc* pDataDesc = nullptr;
        m_PinIdToDataDesc.TryGetValue(GetDataOutput(dataInput).m_uiId, pDataDesc);
        if (pDataDesc == nullptr)
          return VisitorResult::Error;

        --pDataDesc->m_uiUsageCounter;
        if (pDataDesc->m_uiUsageCounter == 0 && pDataDesc->m_DataOffset.IsLocal())
        {
          freeDataOffsets.PushBack(pDataDesc->m_DataOffset);
        }

        // Make a copy first because Insert() might re-allocate and the pointer might point to dead memory afterwards.
        DataDesc dataDesc = *pDataDesc;
        m_PinIdToDataDesc.Insert(dataInput.m_uiId, dataDesc);
      }

      return VisitorResult::Continue;
    });
}

ezResult ezVisualScriptCompiler::BuildNodeDescriptions(AstNode* pEntryAstNode, ezDynamicArray<ezVisualScriptNodeDescription>& out_NodeDescriptions)
{
  ezHashTable<const AstNode*, ezUInt32> astNodeToNodeDescIndices;
  out_NodeDescriptions.Clear();

  auto CreateNodeDesc = [&](const AstNode& astNode, ezUInt32& out_uiNodeDescIndex) -> ezResult {
    out_uiNodeDescIndex = out_NodeDescriptions.GetCount();

    auto& nodeDesc = out_NodeDescriptions.ExpandAndGetRef();
    nodeDesc.m_Type = astNode.m_Type;
    nodeDesc.m_DeductedDataType = astNode.m_DeductedDataType;
    nodeDesc.m_sTargetTypeName = astNode.m_sTargetTypeName;
    nodeDesc.m_Value = astNode.m_Value;

    for (auto& dataInput : astNode.m_Inputs)
    {
      DataDesc dataDesc;
      m_PinIdToDataDesc.TryGetValue(dataInput.m_uiId, dataDesc);
      nodeDesc.m_InputDataOffsets.PushBack(dataDesc.m_DataOffset);
    }

    for (auto& dataOutput : astNode.m_Outputs)
    {
      DataDesc dataDesc;
      m_PinIdToDataDesc.TryGetValue(dataOutput.m_uiId, dataDesc);
      nodeDesc.m_OutputDataOffsets.PushBack(dataDesc.m_DataOffset);
    }

    astNodeToNodeDescIndices.Insert(&astNode, out_uiNodeDescIndex);
    return EZ_SUCCESS;
  };

  ezUInt32 uiNodeDescIndex = 0;
  EZ_SUCCEED_OR_RETURN(CreateNodeDesc(*pEntryAstNode, uiNodeDescIndex));

  return TraverseAst(pEntryAstNode, ConnectionType::Execution,
    [&](const Connection& connection) {
      ezUInt32 uiCurrentIndex = 0;
      EZ_VERIFY(astNodeToNodeDescIndices.TryGetValue(connection.m_pCurrent, uiCurrentIndex), "Implementation error");
      auto pNodeDesc = &out_NodeDescriptions[uiCurrentIndex];
      if (pNodeDesc->m_ExecutionIndices.GetCount() == connection.m_pCurrent->m_Next.GetCount())
      {
        return VisitorResult::Continue;
      }

      for (auto pNextAstNode : connection.m_pCurrent->m_Next)
      {
        if (pNextAstNode == nullptr)
        {
          pNodeDesc->m_ExecutionIndices.PushBack(static_cast<ezUInt16>(ezInvalidIndex));
        }
        else
        {
          ezUInt32 uiNextIndex = 0;
          if (astNodeToNodeDescIndices.TryGetValue(pNextAstNode, uiNextIndex) == false)
          {
            if (CreateNodeDesc(*pNextAstNode, uiNextIndex).Failed())
              return VisitorResult::Error;

            // array might have been resized, fetch node desc again
            pNodeDesc = &out_NodeDescriptions[uiCurrentIndex];
          }

          pNodeDesc->m_ExecutionIndices.PushBack(uiNextIndex);
        }
      }

      return VisitorResult::Continue;
    });
}

ezResult ezVisualScriptCompiler::TraverseAst(AstNode* pEntryAstNode, ezUInt32 uiConnectionTypes, AstNodeVisitorFunc func)
{
  m_ReportedConnections.Clear();
  ezHybridArray<AstNode*, 64> nodeStack;

  if ((uiConnectionTypes & ConnectionType::Execution) != 0)
  {
    Connection connection = {nullptr, pEntryAstNode, ConnectionType::Execution, ezInvalidIndex};
    auto res = func(connection);
    if (res == VisitorResult::Stop)
      return EZ_SUCCESS;
    if (res == VisitorResult::Error)
      return EZ_FAILURE;
  }

  nodeStack.PushBack(pEntryAstNode);

  while (nodeStack.IsEmpty() == false)
  {
    AstNode* pCurrentAstNode = nodeStack.PeekBack();
    nodeStack.PopBack();

    if ((uiConnectionTypes & ConnectionType::Data) != 0)
    {
      for (ezUInt32 i = 0; i < pCurrentAstNode->m_Inputs.GetCount(); ++i)
      {
        auto& dataInput = pCurrentAstNode->m_Inputs[i];

        if (dataInput.m_pSourceNode == nullptr)
          continue;

        Connection connection = {pCurrentAstNode, dataInput.m_pSourceNode, ConnectionType::Data, i};
        if (m_ReportedConnections.Insert(connection))
          continue;

        auto res = func(connection);
        if (res == VisitorResult::Stop)
          continue;
        if (res == VisitorResult::Error)
          return EZ_FAILURE;

        nodeStack.PushBack(dataInput.m_pSourceNode);
      }
    }

    if ((uiConnectionTypes & ConnectionType::Execution) != 0)
    {
      for (ezUInt32 i = 0; i < pCurrentAstNode->m_Next.GetCount(); ++i)
      {
        auto pNextAstNode = pCurrentAstNode->m_Next[i];
        EZ_ASSERT_DEBUG(pNextAstNode != pCurrentAstNode, "");

        if (pNextAstNode == nullptr)
          continue;

        Connection connection = {pCurrentAstNode, pNextAstNode, ConnectionType::Execution, i};
        if (m_ReportedConnections.Insert(connection))
          continue;

        auto res = func(connection);
        if (res == VisitorResult::Stop)
          continue;
        if (res == VisitorResult::Error)
          return EZ_FAILURE;

        nodeStack.PushBack(pNextAstNode);
      }
    }
  }

  return EZ_SUCCESS;
}

ezResult ezVisualScriptCompiler::FinalizeDataOffsets()
{
  m_Module.m_InstanceDataDesc.CalculatePerTypeStartOffsets();
  m_Module.m_ConstantDataDesc.CalculatePerTypeStartOffsets();

  auto GetDataDesc = [this](const CompiledFunction& function, DataOffset dataOffset) -> const ezVisualScriptDataDescription* {
    switch (dataOffset.GetSource())
    {
      case DataOffset::Source::Local:
        return &function.m_LocalDataDesc;
      case DataOffset::Source::Instance:
        return &m_Module.m_InstanceDataDesc;
      case DataOffset::Source::Constant:
        return &m_Module.m_ConstantDataDesc;
        EZ_DEFAULT_CASE_NOT_IMPLEMENTED;
    }

    return nullptr;
  };

  for (auto& function : m_Module.m_Functions)
  {
    function.m_LocalDataDesc.CalculatePerTypeStartOffsets();

    for (auto& nodeDesc : function.m_NodeDescriptions)
    {
      for (auto& dataOffset : nodeDesc.m_InputDataOffsets)
      {
        dataOffset = GetDataDesc(function, dataOffset)->GetOffset(dataOffset.GetType(), dataOffset.m_uiByteOffset, dataOffset.GetSource());
      }

      for (auto& dataOffset : nodeDesc.m_OutputDataOffsets)
      {
        EZ_ASSERT_DEBUG(dataOffset.IsConstant() == false, "Cannot write to constant data");
        dataOffset = GetDataDesc(function, dataOffset)->GetOffset(dataOffset.GetType(), dataOffset.m_uiByteOffset, dataOffset.GetSource());
      }
    }
  }

  for (auto& it : m_Module.m_InstanceDataMapping.m_Content)
  {
    auto& dataOffset = it.Value().m_DataOffset;
    dataOffset = m_Module.m_InstanceDataDesc.GetOffset(dataOffset.GetType(), dataOffset.m_uiByteOffset, dataOffset.GetSource());
  }

  return EZ_SUCCESS;
}

ezResult ezVisualScriptCompiler::FinalizeConstantData()
{
  m_Module.m_ConstantDataStorage.AllocateStorage();

  for (auto& it : m_ConstantDataToIndex)
  {
    const ezVariant& value = it.Key();
    ezUInt32 uiIndex = it.Value();

    auto scriptDataType = ezVisualScriptDataType::FromVariantType(value.GetType());
    if (scriptDataType == ezVisualScriptDataType::Invalid)
    {
      scriptDataType = ezVisualScriptDataType::Variant;
    }

    auto dataOffset = m_Module.m_ConstantDataDesc.GetOffset(scriptDataType, uiIndex, DataOffset::Source::Constant);

    m_Module.m_ConstantDataStorage.SetDataFromVariant(dataOffset, value, 0);
  }

  return EZ_SUCCESS;
}

void ezVisualScriptCompiler::DumpAST(AstNode* pEntryAstNode, ezStringView sOutputPath, ezStringView sFunctionName, ezStringView sSuffix)
{
  if (sOutputPath.IsEmpty())
    return;

  ezDGMLGraph dgmlGraph;
  {
    ezHashTable<const AstNode*, ezUInt32> nodeCache;
    TraverseAst(pEntryAstNode, ConnectionType::All,
      [&](const Connection& connection) {
        ezUInt32 uiGraphNode = 0;
        if (nodeCache.TryGetValue(connection.m_pCurrent, uiGraphNode) == false)
        {
          const char* szTypeName = ezVisualScriptNodeDescription::Type::GetName(connection.m_pCurrent->m_Type);
          float colorX = ezSimdRandom::FloatZeroToOne(ezSimdVec4i(ezHashingUtils::StringHash(szTypeName))).x();

          ezDGMLGraph::NodeDesc nd;
          nd.m_Color = ezColorScheme::LightUI(colorX);
          uiGraphNode = dgmlGraph.AddNode(szTypeName, &nd);
          nodeCache.Insert(connection.m_pCurrent, uiGraphNode);
        }

        if (connection.m_pPrev != nullptr)
        {
          ezUInt32 uiPrevGraphNode = 0;
          EZ_VERIFY(nodeCache.TryGetValue(connection.m_pPrev, uiPrevGraphNode), "");

          if (connection.m_Type == ConnectionType::Execution)
          {
            dgmlGraph.AddConnection(uiPrevGraphNode, uiGraphNode, "Exec");
          }
          else
          {
            auto& dataInput = connection.m_pPrev->m_Inputs[connection.m_uiPrevPinIndex];
            auto& dataOutput = GetDataOutput(dataInput);

            ezStringBuilder sLabel;
            sLabel.Format("o{}:{} (id: {})->i{}:{} (id: {})", dataInput.m_uiSourcePinIndex, ezVisualScriptDataType::GetName(dataOutput.m_DataType), dataOutput.m_uiId, connection.m_uiPrevPinIndex, ezVisualScriptDataType::GetName(dataInput.m_DataType), dataInput.m_uiId);

            dgmlGraph.AddConnection(uiGraphNode, uiPrevGraphNode, sLabel);
          }
        }

        return VisitorResult::Continue;
      })
      .IgnoreResult();
  }

  ezStringView sExt = sOutputPath.GetFileExtension();
  ezStringBuilder sFullPath;
  sFullPath.Append(sOutputPath.GetFileDirectory(), sOutputPath.GetFileName(), "_", sFunctionName, sSuffix);
  sFullPath.Append(".", sExt);

  ezDGMLGraphWriter dgmlGraphWriter;
  if (dgmlGraphWriter.WriteGraphToFile(sFullPath, dgmlGraph).Succeeded())
  {
    ezLog::Info("AST was dumped to: {}", sFullPath);
  }
  else
  {
    ezLog::Error("Failed to dump AST to: {}", sFullPath);
  }
}

void ezVisualScriptCompiler::DumpGraph(ezArrayPtr<const ezVisualScriptNodeDescription> nodeDescriptions, ezStringView sOutputPath, ezStringView sFunctionName, ezStringView sSuffix)
{
  if (sOutputPath.IsEmpty())
    return;

  ezDGMLGraph dgmlGraph;
  {
    ezStringBuilder sTmp;
    for (auto& nodeDesc : nodeDescriptions)
    {
      ezStringView sTypeName = ezVisualScriptNodeDescription::Type::GetName(nodeDesc.m_Type);
      sTmp = sTypeName;

      nodeDesc.AppendUserDataName(sTmp);

      for (auto& dataOffset : nodeDesc.m_InputDataOffsets)
      {
        sTmp.AppendFormat("\n Input {} {}[{}]", DataOffset::Source::GetName(dataOffset.GetSource()), ezVisualScriptDataType::GetName(dataOffset.GetType()), dataOffset.m_uiByteOffset);
      }

      for (auto& dataOffset : nodeDesc.m_OutputDataOffsets)
      {
        sTmp.AppendFormat("\n Output {} {}[{}]", DataOffset::Source::GetName(dataOffset.GetSource()), ezVisualScriptDataType::GetName(dataOffset.GetType()), dataOffset.m_uiByteOffset);
      }

      float colorX = ezSimdRandom::FloatZeroToOne(ezSimdVec4i(ezHashingUtils::StringHash(sTypeName))).x();

      ezDGMLGraph::NodeDesc nd;
      nd.m_Color = ezColorScheme::LightUI(colorX);

      dgmlGraph.AddNode(sTmp, &nd);
    }

    for (ezUInt32 i = 0; i < nodeDescriptions.GetCount(); ++i)
    {
      for (auto uiNextIndex : nodeDescriptions[i].m_ExecutionIndices)
      {
        if (uiNextIndex == ezSmallInvalidIndex)
          continue;

        dgmlGraph.AddConnection(i, uiNextIndex);
      }
    }
  }

  ezStringView sExt = sOutputPath.GetFileExtension();
  ezStringBuilder sFullPath;
  sFullPath.Append(sOutputPath.GetFileDirectory(), sOutputPath.GetFileName(), "_", sFunctionName, sSuffix);
  sFullPath.Append(".", sExt);

  ezDGMLGraphWriter dgmlGraphWriter;
  if (dgmlGraphWriter.WriteGraphToFile(sFullPath, dgmlGraph).Succeeded())
  {
    ezLog::Info("AST was dumped to: {}", sFullPath);
  }
  else
  {
    ezLog::Error("Failed to dump AST to: {}", sFullPath);
  }
}
