#include <EnginePluginScene/EnginePluginScenePCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorPluginScene/Actions/LayerActions.h>
#include <EditorPluginScene/Panels/LayerPanel/LayerAdapter.moc.h>
#include <EditorPluginScene/Scene/Scene2Document.h>
#include <GuiFoundation/UIServices/UIServices.moc.h>
#include <QToolTip>

ezQtLayerAdapter::ezQtLayerAdapter(ezScene2Document* pDocument)
  : ezQtDocumentTreeModelAdapter(pDocument->GetSceneObjectManager(), ezGetStaticRTTI<ezSceneLayer>(), nullptr)
{
  m_pSceneDocument = pDocument;
  m_pSceneDocument->m_LayerEvents.AddEventHandler(
    ezMakeDelegate(&ezQtLayerAdapter::LayerEventHandler, this), m_LayerEventUnsubscriber);

  ezDocument::s_EventsAny.AddEventHandler(ezMakeDelegate(&ezQtLayerAdapter::DocumentEventHander, this), m_DocumentEventUnsubscriber);
}

ezQtLayerAdapter::~ezQtLayerAdapter()
{
  m_LayerEventUnsubscriber.Unsubscribe();
  m_DocumentEventUnsubscriber.Unsubscribe();
}

QVariant ezQtLayerAdapter::data(const ezDocumentObject* pObject, int row, int column, int role) const
{
  switch (role)
  {
    case UserRoles::LayerGuid:
    {
      ezObjectAccessorBase* pAccessor = m_pSceneDocument->GetSceneObjectAccessor();
      ezUuid layerGuid = pAccessor->Get<ezUuid>(pObject, "Layer");
      return QVariant::fromValue(layerGuid);
    }
    break;
    case Qt::DisplayRole:
    case Qt::ToolTipRole:
    {
      ezObjectAccessorBase* pAccessor = m_pSceneDocument->GetSceneObjectAccessor();
      ezUuid layerGuid = pAccessor->Get<ezUuid>(pObject, "Layer");
      // Use curator to get name in case the layer is unloaded and there is no document to query.
      const ezAssetCurator::ezLockedSubAsset subAsset = ezAssetCurator::GetSingleton()->GetSubAsset(layerGuid);
      if (subAsset.isValid())
      {
        if (role == Qt::ToolTipRole)
        {
          return subAsset->m_pAssetInfo->m_sAbsolutePath.GetData();
        }
        ezStringBuilder sName = subAsset->GetName();
        QString sQtName = QString::fromUtf8(sName.GetData());
        if (ezSceneDocument* pLayer = m_pSceneDocument->GetLayerDocument(layerGuid))
        {
          if (pLayer->IsModified())
          {
            sQtName += "*";
          }
        }
        return sQtName;
      }
      else
      {
        return QStringLiteral("Layer guid not found");
      }
    }
    break;

    case Qt::DecorationRole:
    {
      return ezQtUiServices::GetCachedIconResource(":/EditorPluginScene/Icons/Layer16.png");
    }
    break;
    case Qt::ForegroundRole:
    {
      ezObjectAccessorBase* pAccessor = m_pSceneDocument->GetSceneObjectAccessor();
      ezUuid layerGuid = pAccessor->Get<ezUuid>(pObject, "Layer");
      if (!m_pSceneDocument->IsLayerLoaded(layerGuid))
      {
        return QColor(128, 128, 128);
      }
    }
    break;
    case Qt::FontRole:
    {
      QFont font;
      ezObjectAccessorBase* pAccessor = m_pSceneDocument->GetSceneObjectAccessor();
      ezUuid layerGuid = pAccessor->Get<ezUuid>(pObject, "Layer");
      if (m_pSceneDocument->GetActiveLayer() == layerGuid)
        font.setBold(true);
      return font;
    }
    break;
  }

  return QVariant();
}

bool ezQtLayerAdapter::setData(const ezDocumentObject* pObject, int row, int column, const QVariant& value, int role) const
{
  return false;
}

void ezQtLayerAdapter::LayerEventHandler(const ezScene2LayerEvent& e)
{
  switch (e.m_Type)
  {
    case ezScene2LayerEvent::Type::LayerUnloaded:
    case ezScene2LayerEvent::Type::LayerLoaded:
    {
      QVector<int> v;
      v.push_back(Qt::DisplayRole);
      v.push_back(Qt::ForegroundRole);
      Q_EMIT dataChanged(m_pSceneDocument->GetLayerObject(e.m_layerGuid), v);
    }
    break;
    case ezScene2LayerEvent::Type::ActiveLayerChanged:
    {
      QVector<int> v;
      v.push_back(Qt::FontRole);
      if (auto pObject = m_pSceneDocument->GetLayerObject(m_CurrentActiveLayer))
      {
        Q_EMIT dataChanged(pObject, v);
      }
      Q_EMIT dataChanged(m_pSceneDocument->GetLayerObject(e.m_layerGuid), v);
      m_CurrentActiveLayer = e.m_layerGuid;
    }
    default:
      break;
  }
}

void ezQtLayerAdapter::DocumentEventHander(const ezDocumentEvent& e)
{
  if (e.m_Type == ezDocumentEvent::Type::DocumentSaved || e.m_Type == ezDocumentEvent::Type::ModifiedChanged)
  {
    const ezDocumentObject* pLayerObj = m_pSceneDocument->GetLayerObject(e.m_pDocument->GetGuid());
    if (pLayerObj)
    {
      QVector<int> v;
      v.push_back(Qt::DisplayRole);
      v.push_back(Qt::ForegroundRole);
      Q_EMIT dataChanged(pLayerObj, v);
    }
  }
}

//////////////////////////////////////////////////////////////////////////

ezQtLayerDelegate::ezQtLayerDelegate(QObject* pParent, ezScene2Document* pDocument)
  : ezQtItemDelegate(pParent)
  , m_pDocument(pDocument)
{
}

bool ezQtLayerDelegate::mousePressEvent(QMouseEvent* event, const QStyleOptionViewItem& option, const QModelIndex& index)
{
  const QRect visibleRect = GetVisibleIconRect(option);
  const QRect loadedRect = GetLoadedIconRect(option);
  if (event->button() == Qt::MouseButton::LeftButton && (visibleRect.contains(event->localPos().toPoint()) || loadedRect.contains(event->localPos().toPoint())))
  {
    m_bPressed = true;
    event->accept();
    return true;
  }
  return ezQtItemDelegate::mousePressEvent(event, option, index);
}

bool ezQtLayerDelegate::mouseReleaseEvent(QMouseEvent* event, const QStyleOptionViewItem& option, const QModelIndex& index)
{
  if (m_bPressed)
  {
    const QRect visibleRect = GetVisibleIconRect(option);
    const QRect loadedRect = GetLoadedIconRect(option);
    if (visibleRect.contains(event->localPos().toPoint()))
    {
      const ezUuid layerGuid = index.data(ezQtLayerAdapter::UserRoles::LayerGuid).value<ezUuid>();
      const bool bVisible = !m_pDocument->IsLayerVisible(layerGuid);
      m_pDocument->SetLayerVisible(layerGuid, bVisible).LogFailure();
    }
    else if (loadedRect.contains(event->localPos().toPoint()))
    {
      const ezUuid layerGuid = index.data(ezQtLayerAdapter::UserRoles::LayerGuid).value<ezUuid>();
      if (layerGuid != m_pDocument->GetGuid())
      {
        ezLayerAction::ToggleLayerLoaded(m_pDocument, layerGuid);
      }
    }
    m_bPressed = false;
    event->accept();
    return true;
  }
  return ezQtItemDelegate::mouseReleaseEvent(event, option, index);
}

bool ezQtLayerDelegate::mouseMoveEvent(QMouseEvent* event, const QStyleOptionViewItem& option, const QModelIndex& index)
{
  if (m_bPressed)
  {
    return true;
  }
  return ezQtItemDelegate::mouseMoveEvent(event, option, index);
}

void ezQtLayerDelegate::paint(QPainter* painter, const QStyleOptionViewItem& opt, const QModelIndex& index) const
{
  ezQtItemDelegate::paint(painter, opt, index);

  {
    const ezUuid layerGuid = index.data(ezQtLayerAdapter::UserRoles::LayerGuid).value<ezUuid>();
    if (layerGuid.IsValid())
    {
      {
        const QRect thumbnailRect = GetVisibleIconRect(opt);
        const bool bVisible = m_pDocument->IsLayerVisible(layerGuid);
        const QIcon::Mode mode = bVisible ? QIcon::Mode::Normal : QIcon::Mode::Disabled;
        ezQtUiServices::GetSingleton()->GetCachedIconResource(":/EditorPluginScene/Icons/LayerVisible16.png").paint(painter, thumbnailRect, Qt::AlignmentFlag::AlignCenter, mode);
      }

      if (layerGuid != m_pDocument->GetGuid())
      {
        const QRect thumbnailRect = GetLoadedIconRect(opt);
        const bool bLoaded = m_pDocument->IsLayerLoaded(layerGuid);
        const QIcon::Mode mode = bLoaded ? QIcon::Mode::Normal : QIcon::Mode::Disabled;
        ezQtUiServices::GetSingleton()->GetCachedIconResource(":/EditorPluginScene/Icons/LayerLoaded16.png").paint(painter, thumbnailRect, Qt::AlignmentFlag::AlignCenter, mode);
      }
    }
  }
}

QSize ezQtLayerDelegate::sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& index) const
{
  return ezQtItemDelegate::sizeHint(opt, index);
}

bool ezQtLayerDelegate::helpEvent(QHelpEvent* event, QAbstractItemView* view, const QStyleOptionViewItem& option, const QModelIndex& index)
{
  const ezUuid layerGuid = index.data(ezQtLayerAdapter::UserRoles::LayerGuid).value<ezUuid>();
  if (layerGuid.IsValid())
  {
    const QRect visibleRect = GetVisibleIconRect(option);
    const QRect loadedRect = GetLoadedIconRect(option);
    if (visibleRect.contains(event->pos()))
    {
      const bool bVisible = m_pDocument->IsLayerVisible(layerGuid);
      QToolTip::showText(event->globalPos(), bVisible ? "Hide Layer" : "Show Layer", view);
      return true;
    }
    else if (loadedRect.contains(event->pos()))
    {
      const bool bLoaded = m_pDocument->IsLayerLoaded(layerGuid);
      QToolTip::showText(event->globalPos(), bLoaded ? "Unload Layer" : "Load Layer", view);
      return true;
    }
  }
  return ezQtItemDelegate::helpEvent(event, view, option, index);
}

QRect ezQtLayerDelegate::GetVisibleIconRect(const QStyleOptionViewItem& opt)
{
  return opt.rect.adjusted(opt.rect.width() - opt.rect.height(), 0, 0, 0);
}

QRect ezQtLayerDelegate::GetLoadedIconRect(const QStyleOptionViewItem& opt)
{
  return opt.rect.adjusted(opt.rect.width() - opt.rect.height() * 2, 0, -opt.rect.height(), 0);
}

//////////////////////////////////////////////////////////////////////////

ezQtLayerModel::ezQtLayerModel(ezScene2Document* pDocument)
  : ezQtDocumentTreeModel(pDocument->GetSceneObjectManager(), pDocument->GetSettingsObject()->GetGuid())
  , m_pDocument(pDocument)
{
  m_sTargetContext = "layertree";
}