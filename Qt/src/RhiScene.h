#pragma once
#include "SceneTypes.h"
#include <QImage>
#include <QJsonObject>
#include <QQuickRhiItem>
#include <QStringList>
class Engine;
class RhiScene : public QQuickRhiItem {
    Q_OBJECT
    Q_PROPERTY(Engine *engine READ engine WRITE setEngine NOTIFY engineChanged)
  public:
    explicit RhiScene(QQuickItem *parent = nullptr);
    Engine *engine() const {
        return m_engine;
    }
    void setEngine(Engine *value);
  signals:
    void engineChanged();

  protected:
    QQuickRhiItemRenderer *createRenderer() override;

  private:
    Engine *m_engine = nullptr;
};
QImage renderReference(const QString &root, const QJsonObject &frame, const QJsonObject &item,
                       const QImage &cover, QSize size, const SceneDrawBuffers &buffers = {},
                       quint64 *drawCalls = nullptr);
// 渲染资源由部署步骤生成，属于外部输入；缺失时由调用方拒绝渲染，而不是在渲染线程终止进程。
QStringList renderResourceGaps(const QString &root);
