#pragma once
#include <QQuickRhiItem>

class Engine;

/** Rhine Enhance渲染入口 */
class OpticalScene : public QQuickRhiItem {
    Q_OBJECT
    Q_PROPERTY(Engine *engine READ engine WRITE setEngine NOTIFY engineChanged)
    Q_PROPERTY(
        QQuickItem *originalSource READ originalSource WRITE setOriginalSource NOTIFY originalSourceChanged)
    Q_PROPERTY(
        bool lightingEnabled READ lightingEnabled WRITE setLightingEnabled NOTIFY lightingEnabledChanged)
  public:
    explicit OpticalScene(QQuickItem *parent = nullptr);
    Engine *engine() const {
        return m_engine;
    }
    void setEngine(Engine *engine);
    QQuickItem *originalSource() const {
        return m_originalSource;
    }
    void setOriginalSource(QQuickItem *source);
    bool lightingEnabled() const {
        return m_lightingEnabled;
    }
    void setLightingEnabled(bool enabled);

  signals:
    void engineChanged();
    void originalSourceChanged();
    void lightingEnabledChanged();

  protected:
    QQuickRhiItemRenderer *createRenderer() override;

  private:
    Engine *m_engine = nullptr;
    QQuickItem *m_originalSource = nullptr;
    bool m_lightingEnabled = true;
};
