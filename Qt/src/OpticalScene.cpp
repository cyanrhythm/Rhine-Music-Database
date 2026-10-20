#include "OpticalScene.h"
#include "DiscGeometry.h"
#include "Engine.h"
#include "Glyphs.h"
#include <QFile>
#include <QJsonDocument>
#include <QMatrix4x4>
#include <QPainter>
#include <QSGTexture>
#include <QSGTextureProvider>
// Qt 6.8+ 的 qrhi.h 已提供各后端初始化参数，无需引用不存在的后端头文件。
#include <rhi/qrhi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

// Rhine Enhance
namespace OpticalRendering {
// 同一渲染通道和 GLSL 方程经 Qt Shader Tools 编译为 Metal / HLSL / SPIR-V。
struct PackedMatrix {
    float elements[16];
    PackedMatrix(const QMatrix4x4 &value = QMatrix4x4()) {
        std::copy_n(value.constData(), 16, elements);
    }
};
struct Globals {
    PackedMatrix view, projection, shadow, inverseProjection, aoProjection, aoInverseProjection;
    QVector4D eye, fogColor, fog, viewport, clip;
    PackedMatrix correction;
    QVector4D backend;
    PackedMatrix emitterWorld;
    PackedMatrix emitterInverse;
    QVector4D opticalLight;
    QVector4D emissionColor;
    std::array<QVector4D, 8> actorEdges;
};
struct Surface {
    QVector4D color, physical, optics, attenuation, extra, flags, darkColor, emission;
};
struct Instance {
    PackedMatrix model;
    QVector4D state;
};
static_assert(sizeof(Globals) == 832);
static_assert(sizeof(Surface) == 128);
static_assert(sizeof(Instance) == 80);
static constexpr qint64 MUSIC_TEXTURE_BUDGET_BYTES = 24 * 1024 * 1024;
static QMatrix4x4 matrix(QJsonArray a, int offset = 0) {
    QMatrix4x4 m;
    for (int i = 0; i < 16; ++i) {
        m.data()[i] = float(a[offset + i].toDouble());
    }
    return m;
}
static QVector3D vector(QJsonValue a) {
    auto v = a.toArray();
    return {float(v[0].toDouble()), float(v[1].toDouble()), float(v[2].toDouble())};
}
static float n(QJsonObject o, const char *k) {
    return float(o[k].toDouble());
}
static QVector4D v4(QVector3D v, float w = 0) {
    return {v, w};
}
/** 将取出外壳的八个角投影为凸轮廓，用于合成原版唱片像素；不计算光线或材质。 */
static std::array<QVector4D, 8> projectActorEdges(const QMatrix4x4 &clipFromLocal) {
    QList<QVector2D> points;
    for (const float x : {-2.5f, 2.5f}) {
        for (const float y : {0.f, 3.7f}) {
            for (const float z : {-0.25f, 0.25f}) {
                const auto clip = clipFromLocal * QVector4D(x, y, z, 1);
                points.append({clip.x() / clip.w() * 0.5f + 0.5f, 0.5f - clip.y() / clip.w() * 0.5f});
            }
        }
    }
    std::sort(points.begin(), points.end(), [](const auto &first, const auto &second) {
        return first.x() == second.x() ? first.y() < second.y() : first.x() < second.x();
    });
    const auto cross = [](const QVector2D &first, const QVector2D &second, const QVector2D &third) {
        const auto edge = second - first;
        const auto offset = third - first;
        return edge.x() * offset.y() - edge.y() * offset.x();
    };
    QList<QVector2D> hull;
    for (const auto &point : points) {
        while (hull.size() >= 2 && cross(hull[hull.size() - 2], hull.last(), point) <= 0) {
            hull.removeLast();
        }
        hull.append(point);
    }
    const auto lowerSize = hull.size();
    for (auto iterator = points.crbegin() + 1; iterator != points.crend(); ++iterator) {
        while (hull.size() > lowerSize && cross(hull[hull.size() - 2], hull.last(), *iterator) <= 0) {
            hull.removeLast();
        }
        hull.append(*iterator);
    }
    hull.removeLast();
    std::array<QVector4D, 8> result{};
    for (int index = 0; index < std::min(8, int(hull.size())); ++index) {
        const auto start = hull[index];
        const auto edge = hull[(index + 1) % hull.size()] - start;
        result[index] = {-edge.y(), edge.x(), edge.y() * start.x() - edge.x() * start.y(), 0};
    }
    return result;
}
static QByteArray read(QString root, QString name) {
    QFile f(root + "/" + name);
    if (!f.open(QIODevice::ReadOnly)) {
        qFatal("Missing render resource: %s", qPrintable(name));
    }
    return f.readAll();
}
static QShader shader(QString name) {
    name[0] = name[0].toUpper();
    QFile f(":/optical/optical" + name + ".qsb");
    if (!f.open(QIODevice::ReadOnly)) {
        qFatal("Missing baked shader: %s", qPrintable(name));
    }
    auto s = QShader::fromSerialized(f.readAll());
    if (!s.isValid()) {
        qFatal("Invalid shader");
    }
    return s;
}
struct Mesh {
    QRhiBuffer *vertices = nullptr, *indices = nullptr;
    QJsonArray parts;
};
struct Target {
    QRhiTexture *texture = nullptr;
    QRhiTextureRenderTarget *rt = nullptr;
    QRhiRenderPassDescriptor *rp = nullptr;
};
class Renderer {
  public:
    QRhi *rhi;
    QString root;
    std::vector<std::unique_ptr<QRhiResource>> resources, sized, transient;
    QHash<QString, Mesh> meshes;
    QRhiTexture *surfaceAtlas = nullptr;
    QRhiTexture *lightAtlas = nullptr;
    QRhiTexture *normalAtlas = nullptr;
    QRhiTexture *white = nullptr;
    QRhiTexture *depth = nullptr;
    QRhiBuffer *globalBuffer = nullptr, *floor = nullptr, *coverPlane = nullptr, *musicLabelPlane = nullptr,
               *discBody = nullptr, *discHub = nullptr, *caseDecoration = nullptr;
    int discPrintCount = 0;
    int discBodyCount = 0;
    int discHubCount = 0;
    QRhiTexture *decorationTexture = nullptr, *placeholderCover = nullptr;
    QRhiSampler *linear = nullptr;
    Target color, target;
    QRhiResourceUpdateBatch *updates = nullptr;
    QHash<QString, QRhiGraphicsPipeline *> pipelines;
    QHash<QString, QRhiShaderResourceBindings *> pipelineLayouts;
    struct BufferState {
        QRhiBuffer *buffer = nullptr;
        QByteArray bytes;
        quint64 used = 0;
    };
    struct MaterialState {
        BufferState uniform;
        QRhiShaderResourceBindings *bindings = nullptr;
        QRhiTexture *label = nullptr, *cover = nullptr;
        quint64 used = 0;
    };
    QHash<int, BufferState> instances;
    QHash<QString, MaterialState> materials;
    QHash<QString, QRhiShaderResourceBindings *> postBindings;
    QHash<QString, QString> labelRevisions;
    QHash<QString, qint64> coverRevisions;
    SceneDrawBuffers nativeBuffers;
    quint64 generation = 0;
    std::optional<quint64> cachedGeometryRevision;
    QJsonObject cachedItem;
    qint64 cachedCoverRevision = 0;
    bool cachedLightingEnabled = true;
    bool cachedSelectedHidden = false;
    quint64 bindingCreations = 0;
    quint64 coverUploads = 0;
    quint64 drawCalls = 0;
    QHash<QString, QRhiTexture *> labels, covers;
    QHash<qint64, QRhiTexture *> coverTextures;
    QRhiTexture *labelTexture = nullptr, *coverTexture = nullptr;
    QStringList textureOrder;
    QSet<QString> activeTextureKeys;
    QSize size;
    std::unique_ptr<Glyphs> glyphs;
    QJsonObject musicItem;
    QString revision;
    template <class T> T *own(T *value, bool resize = false, bool temporary = false) {
        (temporary ? transient : resize ? sized : resources).emplace_back(value);
        return value;
    }
    template <class T> T *create(T *value) {
        if (!value->create()) {
            qFatal("RHI resource allocation failed");
        }
        return value;
    }
    QRhiBuffer *buffer(const QByteArray &data, QRhiBuffer::UsageFlags usage, bool temporary = false) {
        // D3D11 不支持非动态 uniform buffer；类型与上传方式必须同时匹配。
        const bool uniform = usage.testFlag(QRhiBuffer::UniformBuffer);
        auto b =
            own(rhi->newBuffer(uniform ? QRhiBuffer::Dynamic : QRhiBuffer::Immutable, usage, data.size()),
                false, temporary);
        if (!b->create()) {
            qFatal("RHI buffer allocation failed: backend=%d uniform=%d size=%lld", int(rhi->backend()),
                   int(uniform), static_cast<long long>(data.size()));
        }
        if (uniform) {
            updates->updateDynamicBuffer(b, 0, data.size(), data.constData());
        } else {
            updates->uploadStaticBuffer(b, data.constData());
        }
        return b;
    }
    QRhiTexture *texture(QSize s, QRhiTexture::Format format, QRhiTexture::Flags flags = {},
                         bool resize = false) {
        return create(own(rhi->newTexture(format, s, 1, flags), resize));
    }
    void upload(QRhiTexture *t, QByteArray bytes) {
        QRhiTextureSubresourceUploadDescription data(bytes);
        updates->uploadTexture(t, QRhiTextureUploadDescription({QRhiTextureUploadEntry(0, 0, data)}));
    }
    Renderer(QString path, QRhi *device) : rhi(device), root(path) {
        updates = rhi->nextResourceUpdateBatch();
        glyphs = std::make_unique<Glyphs>(root);
        globalBuffer =
            create(own(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(Globals))));
        for (auto pair :
             {std::pair{"cassette", "archive-cassette"}, std::pair{"assembly", "archive-assembly"}}) {
            meshes[pair.first] = {
                buffer(read(root, QString(pair.second) + ".bin"), QRhiBuffer::VertexBuffer),
                buffer(read(root, QString(pair.second) + "-indices.bin"), QRhiBuffer::IndexBuffer),
                QJsonDocument::fromJson(read(root, QString(pair.second) + ".json"))
                    .object()["meshes"]
                    .toArray()};
        }
        auto sampler = [&](QRhiSampler::Filter filter, QRhiSampler::Filter mip,
                           QRhiSampler::AddressMode address) {
            return create(own(rhi->newSampler(filter, filter, mip, address, address)));
        };
        linear = sampler(QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge);
        surfaceAtlas = texture({576, 192}, QRhiTexture::RGBA16F);
        upload(surfaceAtlas, read(root, "archive-baked-surface.bin"));
        lightAtlas = texture({384, 512}, QRhiTexture::RGBA16F);
        upload(lightAtlas, read(root, "archive-baked-light.bin"));
        normalAtlas = texture({64, 128}, QRhiTexture::RGBA16F);
        upload(normalAtlas, read(root, "archive-baked-normal.bin"));
        white = texture({1, 1}, QRhiTexture::RGBA8);
        upload(white, QByteArray(4, char(255)));
        floor = plane(-100, -100, 100, 100, 0, 0, 0, 0, 0);
        // 盘面、盘边和中心透明环共享外壳中心；原网格及变换保持不变。
        auto print =
            DiscGeometry::annulus(DiscGeometry::HubRadius, DiscGeometry::PrintRadius, .105f, .105f, false);
        auto body = DiscGeometry::annulus(DiscGeometry::HoleRadius, DiscGeometry::Radius, .10f, .055f);
        auto hub = DiscGeometry::annulus(DiscGeometry::HoleRadius, DiscGeometry::HubRadius, .112f, .055f);
        coverPlane = buffer(print.bytes(), QRhiBuffer::VertexBuffer);
        discBody = buffer(body.bytes(), QRhiBuffer::VertexBuffer);
        discHub = buffer(hub.bytes(), QRhiBuffer::VertexBuffer);
        discPrintCount = print.count();
        discBodyCount = body.count();
        discHubCount = hub.count();
        musicLabelPlane = plane(-.83, .07, -.03, .29, 0, -.83, .07, .8, .22);
        caseDecoration = plane(-2.5, 0, 2.5, 3.7, .222, -2.5, 0, 5, 3.7);
        QImage decoration(1000, 740, QImage::Format_RGBA8888_Premultiplied);
        decoration.fill(Qt::transparent);
        QPainter painter(&decoration);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor ink(65, 70, 62, 170), accent(119, 123, 96, 195);
        painter.setPen(QPen(ink, 1.4));
        for (auto point : {QPointF(75, 60), QPointF(930, 60), QPointF(75, 685), QPointF(930, 685)}) {
            painter.drawLine(point + QPointF(-10, 0), point + QPointF(10, 0));
            painter.drawLine(point + QPointF(0, -10), point + QPointF(0, 10));
        }
        glyphs->text(painter, "MUSIC ARCHIVE / 001", 130, 688, 15, "regular", .5, ink);
        painter.save();
        painter.translate(75, 465);
        painter.rotate(-90);
        glyphs->text(painter, "RHINE LAB", 0, 0, 21, "demibold", 2, ink);
        painter.restore();
        painter.drawEllipse(QRectF(875, 574, 55, 55));
        painter.drawEllipse(QRectF(891, 574, 23, 55));
        painter.drawLine(875, 601, 930, 601);
        painter.fillRect(QRectF(875, 654, 25, 5), accent);
        painter.end();
        decorationTexture = texture(decoration.size(), QRhiTexture::RGBA8);
        updates->uploadTexture(decorationTexture, decoration);
        QImage placeholder(512, 512, QImage::Format_RGBA8888_Premultiplied);
        placeholder.fill(QColor(94, 115, 112));
        QPainter placeholderPainter(&placeholder);
        placeholderPainter.setRenderHint(QPainter::Antialiasing);
        placeholderPainter.setPen(QPen(QColor(185, 196, 178), 2));
        placeholderPainter.drawEllipse(QRectF(52, 52, 408, 408));
        glyphs->text(placeholderPainter, "RHINE LAB", 130, 340, 34, "demibold", 1, QColor(215, 220, 204));
        placeholderPainter.end();
        placeholderCover = texture(placeholder.size(), QRhiTexture::RGBA8);
        updates->uploadTexture(placeholderCover, placeholder);
    }
    ~Renderer() {
        if (updates) {
            updates->release();
        }
    }
    QRhiBuffer *plane(float x0, float y0, float x1, float y1, float z, float ux, float uy, float uw,
                      float uh) {
        std::vector<float> vertices;
        for (auto v : {QVector2D{x0, y0}, QVector2D{x1, y0}, QVector2D{x1, y1}, QVector2D{x0, y0},
                       QVector2D{x1, y1}, QVector2D{x0, y1}}) {
            float data[] = {
                v.x(), v.y(), z, 1, 0, 0, 1, 0, uw ? (v.x() - ux) / uw : 0, uh ? (v.y() - uy) / uh : 0, 0, 0};
            vertices.insert(vertices.end(), std::begin(data), std::end(data));
        }
        return buffer(QByteArray(reinterpret_cast<const char *>(vertices.data()), vertices.size() * 4),
                      QRhiBuffer::VertexBuffer);
    }
    Target makeTarget(QRhiTexture *texture, QRhiTexture *depth = nullptr, QRhiTexture *second = nullptr) {
        QRhiTextureRenderTargetDescription desc;
        if (texture) {
            if (second) {
                desc.setColorAttachments({QRhiColorAttachment(texture), QRhiColorAttachment(second)});
            } else {
                desc.setColorAttachments({QRhiColorAttachment(texture)});
            }
        }
        desc.setDepthTexture(depth);
        auto rt = own(rhi->newTextureRenderTarget(desc), true);
        auto rp = own(rt->newCompatibleRenderPassDescriptor(), true);
        rt->setRenderPassDescriptor(rp);
        create(rt);
        return {texture, rt, rp};
    }
    void invalidateBindings(QRhiTexture *texture = nullptr) {
        for (auto &state : materials) {
            // 换曲只影响采样该纹理的材质；窗口尺寸变化才需要整批重新绑定。
            if (texture && state.label != texture && state.cover != texture) {
                continue;
            }
            retire(state.bindings);
            state.bindings = nullptr;
        }
    }
    /** 仅分配颜色和深度；烘焙增强不创建实时透射、阴影、AO、景深或光晕目标。 */
    void resize(QSize value, bool needsReadbackTarget) {
        if (size == value) {
            return;
        }
        invalidateBindings();
        postBindings.clear();
        pipelines.clear();
        pipelineLayouts.clear();
        sized.clear();
        size = value;
        cachedGeometryRevision.reset();
        auto rtFlags = QRhiTexture::RenderTarget;
        depth = texture(size, QRhiTexture::D32F, rtFlags, true);
        color = makeTarget(texture(size, QRhiTexture::RGBA16F, rtFlags, true), depth);
        target = {};
        if (needsReadbackTarget) {
            target = makeTarget(texture(size, QRhiTexture::RGBA8, rtFlags, true));
        }
    }
    /** 从选中壳体的原生矩阵定位光源，并将场景参数复制到本分支的独立常量缓冲区。 */
    Globals globals(QJsonObject f) {
        auto projection = matrix(f["projection"].toArray());
        auto theme = n(f["draws"].toArray().first().toObject(), "theme");
        QMatrix4x4 emitter;
        bool hasEmitter = false;
        for (const auto &entry : f["draws"].toArray()) {
            const auto draw = entry.toObject();
            if (draw["name"] == "Frosted_Polymer" && draw["musicSelected"].toBool() &&
                !draw["shadowOnly"].toBool()) {
                const auto bytes = matrixBytes(draw);
                if (bytes.size() >= 64) {
                    memcpy(emitter.data(), bytes.constData(), 64);
                    hasEmitter = true;
                    break;
                }
            }
        }
        const float lightPower = hasEmitter && f["opticalLightEnabled"].toBool(true) ? 1.f : 0.f;
        const bool originalActorAvailable = f["originalActorAvailable"].toBool() && hasEmitter;
        std::array<QVector4D, 8> actorEdges{};
        if (originalActorAvailable) {
            actorEdges = projectActorEdges(projection * matrix(f["view"].toArray()) * emitter);
        }
        return {matrix(f["view"].toArray()),
                projection,
                {},
                {},
                {},
                {},
                v4(vector(f["eye"])),
                v4(vector(f["fogColor"])),
                {n(f, "fogNear"), n(f, "fogFar"), n(f, "focus"), n(f, "aperture")},
                {float(size.width()), float(size.height()), theme, f["viewer"].toBool() ? 1.f : 0.f},
                {n(f, "near"), n(f, "far"), n(f, "detail"), f["reference"].toBool() ? 1.f : 0.f},
                rhi->clipSpaceCorrMatrix(),
                {rhi->isYUpInFramebuffer() ? 1.f : 0.f, 0, 0, 0},
                emitter,
                emitter.inverted(),
                {lightPower, f["viewer"].toBool() ? 0.15f : 1.f, 0.62f,
                 originalActorAvailable ? float(f["originalActorBlend"].toDouble()) : 0.f},
                {1.f, 0.72f, 0.38f, 1.f},
                actorEdges};
    }
    QVector3D dark(QString name) {
        static const QHash<QString, uint32_t> colors = {
            {"Frosted_Polymer", 0x626b70},    {"Ivory_Edges", 0x687277},   {"Optical_Diffuser", 0x192226},
            {"Titanium_Fasteners", 0xb1b9bb}, {"Index_Inlay", 0xc6a36b},   {"Printed_Label", 0x303a3e},
            {"Subsurface_Optics", 0x939e9f},  {"Optical_Edges", 0xbbc3bc}, {"Carbon_Ink", 0xb6bdb8}};
        auto hex = colors.value(name, name.contains("Orange") ? 0xbb8850 : 0x969f9f);
        auto channel = [](uint32_t n) {
            float v = n / 255.f;
            return v <= .04045f ? v / 12.92f : float(pow((v + .055f) / 1.055f, 2.4f));
        };
        return {channel((hex >> 16) & 255), channel((hex >> 8) & 255), channel(hex & 255)};
    }
    Surface surface(QJsonObject d) {
        auto name = d["name"].toString();
        bool optical = name.startsWith("Optical_Glass") || name == "Optical_Bridge_Glass";
        float type = d["mesh"].toInt() == -2                            ? 6
                     : d["label"].toBool()                              ? 1
                     : name == "Frosted_Polymer"                        ? 3
                     : name == "Ivory_Edges"                            ? 4
                     : name == "Optical_Diffuser"                       ? 5
                     : !d["instanced"].toBool() && !d["floor"].toBool() ? 2
                                                                        : 0;
        return {v4(vector(d["color"]), n(d, "opacity")),
                {n(d, "roughness"), n(d, "metalness"), n(d, "transmission"), n(d, "thickness")},
                {n(d, "ior"), n(d, "attenuationDistance"), name == "Index_Inlay" ? 1.f : 0.f,
                 d["sharpCover"].toBool() ? 1.f : 0.f},
                v4(vector(d["attenuation"])),
                {n(d, "clearcoat"), n(d, "clearcoatRoughness"),
                 type == 6 ? float(std::cos(n(d, "discAngle"))) : (optical ? 1.f : 0.f),
                 type == 6 ? float(std::sin(n(d, "discAngle"))) : 0.f},
                {type, n(d, "side"), n(d, "subdued"), d["floor"].toBool() ? 1.f : 0.f},
                v4(dark(name), d["instanced"].toBool() ? 1.f : 0.f),
                {d["musicSelected"].toBool() ? 1.f : 0.f, 0, 0, 0}};
    }
    QList<QJsonObject> musicDraws(QJsonObject f) {
        static const QStringList removed = {"Internal_Ceramic",  "Optical_Edges",       "Subsurface_Optics",
                                            "Optical_Film_Edge", "Amber_Optical_Inlay", "Optical_Film",
                                            "Champagne_Index",   "Printed_Label"};
        QList<QJsonObject> result;
        for (auto v : f["draws"].toArray()) {
            auto d = v.toObject();
            const auto name = d["name"].toString();
            if (removed.contains(name)) {
                continue;
            }
            const bool shell =
                name == "Frosted_Polymer" || name == "Ivory_Edges" || name == "Optical_Diffuser";
            if (name == "Titanium_Fasteners") {
                // 金属反射的暗底色也固定烘焙，避免取消实时环境光后螺钉变成白色圆点。
                d["color"] = QJsonArray{0.065, 0.075, 0.08};
            }
            if (shell) {
                // 固定透光外观来自烘焙图集；只有选中外壳用普通透明混合显示动态封面。
                const bool selectedShell = name == "Frosted_Polymer" && d["musicSelected"].toBool();
                d["transmission"] = 0;
                d["side"] = 2;
                d["transparent"] = selectedShell;
                d["depthWrite"] = !selectedShell;
                double opacity = 1;
                if (selectedShell) {
                    opacity = f["mode"] == "detail" ? 0.20 : 0.62;
                    if (f["viewer"].toBool()) {
                        opacity = 0.14;
                    }
                }
                d["opacity"] = opacity;
            }
            result << d;
            // 密集背景采用透光档案壳，不在每个占位实例中塞入不透明的白色盘体。
            if (name != "Frosted_Polymer" || d["shadowOnly"].toBool() || d["instanced"].toBool()) {
                continue;
            }
            auto part = d;
            part.remove("sharpCover");
            part["color"] = QJsonArray{1, 1, 1};
            part["roughness"] = .36;
            part["metalness"] = 0;
            part["transmission"] = 0;
            part["thickness"] = 0;
            part["opacity"] = 1;
            part["clearcoat"] = .12;
            part["clearcoatRoughness"] = .25;
            part["transparent"] = false;
            part["depthWrite"] = true;
            part["order"] = 0;
            part["side"] = 2;
            part["castShadow"] = false;
            part["discAngle"] = d["musicSelected"].toBool() ? f["discAngle"].toDouble() : 0;
            part["name"] = "Music_Disc_Print";
            part["mesh"] = -2;
            result << part;
            part["name"] = "Music_Disc_Silver";
            part["mesh"] = -3;
            part["color"] = QJsonArray{.68, .72, .74};
            part["metalness"] = .82;
            part["roughness"] = .22;
            result << part;
            part["name"] = "Music_Disc_Hub";
            part["mesh"] = -4;
            part["color"] = QJsonArray{.96, .97, .96};
            part["metalness"] = 0;
            part["transmission"] = .65;
            part["thickness"] = .02;
            part["roughness"] = .13;
            part["depthWrite"] = false;
            result << part;
            part["name"] = "Music_Case_Marks";
            part["mesh"] = -5;
            part["transmission"] = 0;
            part["transparent"] = true;
            part["label"] = true;
            part["order"] = 50;
            result << part;
        }
        return result;
    }
    void retire(QRhiResource *texture) {
        if (!texture) {
            return;
        }
        auto it =
            std::find_if(resources.begin(), resources.end(), [&](auto &v) { return v.get() == texture; });
        if (it != resources.end()) {
            it->release()->deleteLater();
            resources.erase(it);
        }
    }
    void releaseCover(const QString &key) {
        const auto revision = coverRevisions.take(key);
        const auto previous = covers.take(key);
        // 同专辑曲目及无封面的曲目共享纹理，最后一个引用退出后才退休 GPU 对象。
        if (!previous || covers.values().contains(previous)) {
            return;
        }
        invalidateBindings(previous);
        coverTextures.remove(revision);
        retire(previous);
    }
    void updateMusic(const QJsonObject &value, QImage image) {
        const auto key = value["key"].toString("empty");
        const auto labelRevision = value["title"].toString() + "|" + value["artist"].toString() + "|" +
                                   value["kind"].toString() + "|" + value["id"].toString();
        const qint64 coverRevision = image.cacheKey();
        musicItem = value;
        textureOrder.removeAll(key);
        textureOrder << key;
        if (!labels.contains(key) || labelRevisions.value(key) != labelRevision) {
            if (const auto previous = labels.value(key)) {
                invalidateBindings(previous);
            }
            // 覆盖同一 key 时退休的是该 key 的旧对象，而非上一首当前曲目的对象。
            retire(labels.take(key));
            const auto label = glyphs->label(value);
            labels[key] = texture(label.size(), QRhiTexture::RGBA8);
            updates->uploadTexture(labels[key], label);
            labelRevisions[key] = labelRevision;
        }
        if (!covers.contains(key) || (!image.isNull() && coverRevisions.value(key) != coverRevision)) {
            releaseCover(key);
            if (!coverTextures.contains(coverRevision)) {
                // 高分辨率封面直接进入贴图
                const auto coverSize = image.isNull() ? 512 : std::clamp(image.width(), 512, 1024);
                QImage pixels(coverSize, coverSize, QImage::Format_RGBA8888_Premultiplied);
                pixels.fill(QColor::fromRgbF(.36, .45, .45));
                QPainter painter(&pixels);
                painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
                if (!image.isNull()) {
                    const auto dimensions =
                        image.size().scaled(coverSize, coverSize, Qt::KeepAspectRatioByExpanding);
                    painter.drawImage(QRectF((coverSize - dimensions.width()) / 2.,
                                             (coverSize - dimensions.height()) / 2., dimensions.width(),
                                             dimensions.height()),
                                      image);
                } else {
                    painter.setPen(QPen(QColor::fromRgbF(.7, .72, .64), 2));
                    painter.drawEllipse(QRectF(52, 52, 408, 408));
                    glyphs->text(painter, "RHINE LAB", 156, 370, 28, "demibold", 1, QColor(215, 220, 204));
                }
                painter.end();
                const auto cover = texture(pixels.size(), QRhiTexture::RGBA8);
                updates->uploadTexture(cover, pixels);
                coverTextures[coverRevision] = cover;
                ++coverUploads;
            }
            covers[key] = coverTextures.value(coverRevision);
            coverRevisions[key] = coverRevision;
        }
        labelTexture = labels.value(key);
        coverTexture = covers.value(key);
        // 按实际 RGBA8 像素占用限制缓存；仍在动画中的纹理只在归位后参与淘汰。
        while (textureOrder.size() > 32 || musicTextureBytes() > MUSIC_TEXTURE_BUDGET_BYTES) {
            const auto candidate =
                std::find_if(textureOrder.begin(), textureOrder.end(),
                             [&](const auto &key) { return !activeTextureKeys.contains(key); });
            if (candidate == textureOrder.end()) {
                break;
            }
            const auto old = *candidate;
            textureOrder.erase(candidate);
            if (const auto label = labels.value(old)) {
                invalidateBindings(label);
            }
            retire(labels.take(old));
            releaseCover(old);
            labelRevisions.remove(old);
        }
    }
    qint64 musicTextureBytes() const {
        qint64 bytes = 0;
        for (const auto texture : labels) {
            const auto dimensions = texture->pixelSize();
            bytes += qint64(dimensions.width()) * dimensions.height() * 4;
        }
        for (const auto texture : coverTextures) {
            const auto dimensions = texture->pixelSize();
            bytes += qint64(dimensions.width()) * dimensions.height() * 4;
        }
        return bytes;
    }
    QRhiShaderResourceBindings *bindings(std::vector<QRhiShaderResourceBinding> list,
                                         bool temporary = false) {
        ++bindingCreations;
        auto b = own(rhi->newShaderResourceBindings(), false, temporary);
        b->setBindings(list.begin(), list.end());
        return create(b);
    }
    QRhiGraphicsPipeline *pipeline(QString kind, QRhiShaderResourceBindings *srb,
                                   QRhiRenderPassDescriptor *rp, bool back = false, bool doubleSided = false,
                                   bool depthWrite = true, bool blend = false) {
        auto key = kind + QString("/%1/%2/%3/%4/%5")
                              .arg(quintptr(rp))
                              .arg(back)
                              .arg(doubleSided)
                              .arg(depthWrite)
                              .arg(blend);
        if (pipelines.contains(key)) {
            return pipelines[key];
        }
        auto p = own(rhi->newGraphicsPipeline(), true);
        const bool geo = kind == "surface";
        p->setShaderStages({{QRhiShaderStage::Vertex, shader(geo ? "geometry.vert" : "quad.vert")},
                            {QRhiShaderStage::Fragment, shader(kind + ".frag")}});
        if (geo) {
            QRhiVertexInputLayout layout;
            layout.setBindings({{48}, {80, QRhiVertexInputBinding::PerInstance}});
            layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float4, 0},
                                  {0, 1, QRhiVertexInputAttribute::Float4, 16},
                                  {0, 2, QRhiVertexInputAttribute::Float4, 32},
                                  {1, 3, QRhiVertexInputAttribute::Float4, 0},
                                  {1, 4, QRhiVertexInputAttribute::Float4, 16},
                                  {1, 5, QRhiVertexInputAttribute::Float4, 32},
                                  {1, 6, QRhiVertexInputAttribute::Float4, 48},
                                  {1, 7, QRhiVertexInputAttribute::Float4, 64}});
            p->setVertexInputLayout(layout);
            p->setDepthTest(true);
            p->setDepthWrite(depthWrite);
            p->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
            p->setCullMode(back          ? QRhiGraphicsPipeline::Front
                           : doubleSided ? QRhiGraphicsPipeline::None
                                         : QRhiGraphicsPipeline::Back);
            p->setFrontFace(QRhiGraphicsPipeline::CCW);
        }
        QRhiGraphicsPipeline::TargetBlend b;
        if (blend) {
            b.enable = true;
            b.srcColor = QRhiGraphicsPipeline::SrcAlpha;
            b.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            b.srcAlpha = QRhiGraphicsPipeline::One;
            b.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        }
        p->setTargetBlends({b});
        if (!pipelineLayouts.contains(kind)) {
            auto layout = own(rhi->newShaderResourceBindings(), true);
            const auto stages =
                QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
            std::vector<QRhiShaderResourceBinding> values = {
                QRhiShaderResourceBinding::uniformBuffer(0, stages, globalBuffer)};
            if (geo) {
                values.push_back(QRhiShaderResourceBinding::uniformBuffer(1, stages, globalBuffer));
                values.push_back(QRhiShaderResourceBinding::sampledTexture(
                    2, QRhiShaderResourceBinding::FragmentStage, surfaceAtlas, linear));
                values.push_back(QRhiShaderResourceBinding::sampledTexture(
                    3, QRhiShaderResourceBinding::FragmentStage, lightAtlas, linear));
                values.push_back(QRhiShaderResourceBinding::sampledTexture(
                    4, QRhiShaderResourceBinding::FragmentStage, white, linear));
                values.push_back(QRhiShaderResourceBinding::sampledTexture(
                    5, QRhiShaderResourceBinding::FragmentStage, white, linear));
                values.push_back(QRhiShaderResourceBinding::sampledTexture(
                    6, QRhiShaderResourceBinding::FragmentStage, normalAtlas, linear));
            } else {
                values.push_back(QRhiShaderResourceBinding::sampledTexture(
                    2, QRhiShaderResourceBinding::FragmentStage, color.texture, linear));
                values.push_back(QRhiShaderResourceBinding::sampledTexture(
                    3, QRhiShaderResourceBinding::FragmentStage, white, linear));
            }
            layout->setBindings(values.begin(), values.end());
            create(layout);
            pipelineLayouts[kind] = layout;
        }
        p->setSampleCount(1);
        p->setShaderResourceBindings(pipelineLayouts[kind]);
        p->setRenderPassDescriptor(rp);
        create(p);
        return pipelines[key] = p;
    }
    QRhiBuffer *dynamicBuffer(BufferState &state, const QByteArray &bytes, QRhiBuffer::UsageFlags usage) {
        if (!state.buffer || state.buffer->size() < bytes.size()) {
            retire(state.buffer);
            const int capacity = qMax(256, int(bytes.size()) * 2);
            state.buffer = create(own(rhi->newBuffer(QRhiBuffer::Dynamic, usage, capacity)));
            state.bytes.clear();
        }
        if (state.bytes != bytes) {
            // 单个连续区间覆盖实际变化；容量增长之外不再创建每帧临时 GPU 对象。
            qsizetype first = 0, last = bytes.size();
            if (state.bytes.size() == bytes.size()) {
                while (first < last && state.bytes[first] == bytes[first]) {
                    ++first;
                }
                while (last > first && state.bytes[last - 1] == bytes[last - 1]) {
                    --last;
                }
            }
            first = (first / 4) * 4;
            last = qMin(bytes.size(), ((last + 3) / 4) * 4);
            if (last > first) {
                updates->updateDynamicBuffer(state.buffer, int(first), int(last - first),
                                             bytes.constData() + first);
            }
            state.bytes = bytes;
        }
        state.used = generation;
        return state.buffer;
    }
    QByteArray matrixBytes(const QJsonObject &draw) const {
        const auto native = nativeBuffers.constFind(draw["id"].toInt());
        if (native != nativeBuffers.constEnd()) {
            return native->matrices;
        }
        QByteArray result;
        for (const auto &value : draw["matrices"].toArray()) {
            const float number = float(value.toDouble());
            result.append(reinterpret_cast<const char *>(&number), 4);
        }
        return result;
    }
    QRhiBuffer *instance(QJsonObject draw) {
        auto &state = instances[draw["id"].toInt()];
        if (state.used == generation) {
            return state.buffer;
        }
        const auto matrices = matrixBytes(draw);
        const auto native = nativeBuffers.value(draw["id"].toInt());
        const auto themes = draw["themes"].toArray();
        std::vector<Instance> values;
        values.reserve(draw["count"].toInt());
        for (int i = 0; i < draw["count"].toInt(); ++i) {
            Instance value;
            if ((i + 1) * 64 <= matrices.size()) {
                memcpy(value.model.elements, matrices.constData() + i * 64, 64);
            }
            float theme = n(draw, "theme");
            if ((i + 1) * 4 <= native.themes.size()) {
                memcpy(&theme, native.themes.constData() + i * 4, 4);
            } else if (i < themes.size()) {
                theme = float(themes[i].toDouble());
            }
            value.state = {n(draw, "quality"), n(draw, "clarity"), theme, 0};
            values.push_back(value);
        }
        const QByteArray bytes(reinterpret_cast<const char *>(values.data()),
                               values.size() * sizeof(Instance));
        return dynamicBuffer(state, bytes, QRhiBuffer::VertexBuffer);
    }
    struct Draw {
        QJsonObject data;
        QRhiBuffer *vertices, *indices, *instances;
        QRhiTexture *label;
        QRhiTexture *cover;
        int start;
        int count;
        QRhiShaderResourceBindings *bindings;
    };
    QList<Draw> prepare(const QList<QJsonObject> &draws) {
        QList<Draw> result;
        const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
        for (auto d : draws) {
            if (d["count"].toInt() <= 0) {
                continue;
            }
            int start = 0;
            int count = 6;
            int mesh = d["mesh"].toInt();
            QRhiBuffer *vertices = floor, *indices = nullptr;
            if (mesh == -2) {
                vertices = coverPlane;
                count = discPrintCount;
            } else if (mesh == -3) {
                vertices = discBody;
                count = discBodyCount;
            } else if (mesh == -4) {
                vertices = discHub;
                count = discHubCount;
            } else if (mesh == -5) {
                vertices = caseDecoration;
            } else if (mesh >= 0 && meshes.contains(d["asset"].toString()) &&
                       mesh < meshes[d["asset"].toString()].parts.size()) {
                auto m = meshes[d["asset"].toString()];
                auto part = m.parts[mesh].toObject();
                start = part["start"].toInt();
                count = part["count"].toInt();
                vertices = m.vertices;
                indices = m.indices;
            } else if (d["label"].toBool()) {
                vertices = musicLabelPlane;
            } else if (!d["floor"].toBool()) {
                continue;
            }
            const auto resourceKey =
                QString::number(d["id"].toInt()) + "/" + QString::number(mesh) + "/" + d["name"].toString();
            auto &state = materials[resourceKey];
            state.used = generation;
            const auto material = surface(d);
            const auto mb = dynamicBuffer(
                state.uniform, QByteArray(reinterpret_cast<const char *>(&material), sizeof(material)),
                QRhiBuffer::UniformBuffer);
            const auto key =
                d["musicSelected"].toBool() ? musicItem["key"].toString() : d["musicKey"].toString();
            QRhiTexture *label = white;
            if (mesh == -5) {
                label = decorationTexture;
            } else if (d["label"].toBool()) {
                label = labels.value(key, labelTexture ? labelTexture : white);
            }
            // 着色器只有盘面读取封面，只有标签读取文字；其他表面绑定稳定的占位纹理。
            const auto cover = mesh == -2 ? covers.value(key, placeholderCover) : placeholderCover;
            // 缓存命中仍可能换曲，同一个 draw 的绑定必须跟随实际纹理指针。
            if (state.label != label || state.cover != cover) {
                retire(state.bindings);
                state.bindings = nullptr;
                state.label = label;
                state.cover = cover;
            }
            if (!state.bindings) {
                state.bindings = bindings(
                    {QRhiShaderResourceBinding::uniformBuffer(0, stages, globalBuffer),
                     QRhiShaderResourceBinding::uniformBuffer(1, stages, mb),
                     QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                               surfaceAtlas, linear),
                     QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage,
                                                               lightAtlas, linear),
                     QRhiShaderResourceBinding::sampledTexture(4, QRhiShaderResourceBinding::FragmentStage,
                                                               label, linear),
                     QRhiShaderResourceBinding::sampledTexture(5, QRhiShaderResourceBinding::FragmentStage,
                                                               cover, linear),
                     QRhiShaderResourceBinding::sampledTexture(6, QRhiShaderResourceBinding::FragmentStage,
                                                               normalAtlas, linear)},
                    false);
            }
            result.append({d, vertices, indices, instance(d), label, cover, start, count, state.bindings});
        }
        return result;
    }
    QHash<QByteArray, BufferState> batchInstances;
    /**
     * 把同几何、同材质的烘焙背景壳体合并为实例批次。
     * 透明与透射物体保留原排序；每个实例继续使用原来的矩阵、主题和揭示状态。
     */
    QList<Draw> batchDraws(const QList<Draw> &input) {
        QList<Draw> result;
        QList<QByteArray> bytes;
        QList<QByteArray> keys;
        QHash<QByteArray, int> groups;
        QHash<QByteArray, int> groupSlots;
        for (const auto &draw : input) {
            const auto &data = draw.data;
            const auto material = surface(data);
            const bool eligible =
                !data["transparent"].toBool() && material.flags.x() != 2 && data["depthWrite"].toBool();
            if (!eligible) {
                result.append(draw);
                bytes.append(QByteArray());
                keys.append(QByteArray());
                continue;
            }
            auto key = QByteArray("baked/") + QByteArray::number(quintptr(draw.vertices)) + '/' +
                       QByteArray::number(quintptr(draw.indices)) + '/' + QByteArray::number(draw.start) +
                       '/' + QByteArray::number(draw.count) + '/' + QByteArray::number(data["side"].toInt()) +
                       '/' + QByteArray::number(data["order"].toInt()) + '/' +
                       QByteArray::number(quintptr(draw.label)) + '/' +
                       QByteArray::number(quintptr(draw.cover)) + ':';
            const auto bufferKey = key;
            key.append(reinterpret_cast<const char *>(&material), sizeof(material));
            auto group = groups.constFind(key);
            int index;
            if (group == groups.constEnd()) {
                index = result.size();
                groups.insert(key, index);
                result.append(draw);
                result.last().data["count"] = 0;
                bytes.append(QByteArray());
                // 材质插值每帧变化，但批次缓冲区按几何和槽位复用，不能按材质值反复创建。
                const int slot = groupSlots[bufferKey]++;
                keys.append(bufferKey + '/' + QByteArray::number(slot));
            } else {
                index = group.value();
            }
            bytes[index].append(instances.value(data["id"].toInt()).bytes);
            result[index].data["count"] = result[index].data["count"].toInt() + data["count"].toInt();
        }
        for (int i = 0; i < result.size(); ++i) {
            if (!keys[i].isEmpty()) {
                result[i].instances =
                    dynamicBuffer(batchInstances[keys[i]], bytes[i], QRhiBuffer::VertexBuffer);
            }
        }
        return result;
    }
    void geometry(QRhiCommandBuffer *cb, const QList<Draw> &draws) {
        for (const auto &draw : draws) {
            const auto &data = draw.data;
            if (data["shadowOnly"].toBool()) {
                continue;
            }
            const bool hashed = surface(data).flags.x() == 2;
            const bool blend = hashed || data["transparent"].toBool();
            const bool depthWrite = hashed || !blend || data["depthWrite"].toBool();
            cb->setGraphicsPipeline(pipeline("surface", draw.bindings, color.rp, false,
                                             data["side"].toInt() == 2, depthWrite, blend));
            cb->setShaderResources(draw.bindings);
            cb->setViewport({0, 0, float(size.width()), float(size.height())});
            const QRhiCommandBuffer::VertexInput inputs[] = {{draw.vertices, 0}, {draw.instances, 0}};
            cb->setVertexInput(0, 2, inputs, draw.indices, 0, QRhiCommandBuffer::IndexUInt32);
            if (draw.indices) {
                cb->drawIndexed(draw.count, data["count"].toInt(), draw.start);
            } else {
                cb->draw(draw.count, data["count"].toInt(), draw.start);
            }
            ++drawCalls;
        }
    }
    void composite(QRhiCommandBuffer *cb, QRhiRenderTarget *destination, QRhiTexture *originalActor) {
        const auto source = originalActor ? originalActor : white;
        auto &binding = postBindings["final/" + QString::number(quintptr(source))];
        if (!binding) {
            const auto stages =
                QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
            binding = bindings({QRhiShaderResourceBinding::uniformBuffer(0, stages, globalBuffer),
                                QRhiShaderResourceBinding::sampledTexture(
                                    2, QRhiShaderResourceBinding::FragmentStage, color.texture, linear),
                                QRhiShaderResourceBinding::sampledTexture(
                                    3, QRhiShaderResourceBinding::FragmentStage, source, linear)});
        }
        cb->beginPass(destination, Qt::black, {1, 0});
        cb->setGraphicsPipeline(pipeline("finalFragment", binding, destination->renderPassDescriptor()));
        cb->setShaderResources(binding);
        const auto dimensions = destination->pixelSize();
        cb->setViewport({0, 0, float(dimensions.width()), float(dimensions.height())});
        cb->draw(3);
        ++drawCalls;
        cb->endPass();
    }
    /**
     * 仅提交一遍几何和一次最终合成；表面与邻壳柔光只读取构建时烘焙的共享贴图。
     * 帧数据和原生矩阵只读；临时绑定与实例缓冲区必须活到本帧命令提交结束。
     */
    void render(QRhiCommandBuffer *cb, QJsonObject frame, QSize dimensions, QJsonObject item, QImage cover,
                QRhiRenderTarget *finalTarget = nullptr, const Engine::MusicTextures &musicTextures = {},
                QRhiTexture *originalActor = nullptr) {
        transient.clear();
        if (!updates) {
            updates = rhi->nextResourceUpdateBatch();
        }
        resize(dimensions, !finalTarget);
        frame["originalActorAvailable"] = originalActor != nullptr;
        auto g = globals(frame);
        updates->updateDynamicBuffer(globalBuffer, 0, sizeof(g), &g);
        const auto geometryRevision = quint64(frame["geometryRevision"].toDouble());
        const bool sameBackground = cachedGeometryRevision == geometryRevision && cachedItem == item &&
                                    cachedCoverRevision == cover.cacheKey() &&
                                    cachedLightingEnabled == frame["opticalLightEnabled"].toBool(true) &&
                                    cachedSelectedHidden == (g.opticalLight.w() >= 1.f);
        if (g.opticalLight.w() >= 1.f && sameBackground) {
            // 只旋转唱片贴图时，原版提供新唱片像素；重复壳体沿用已绘制的背景颜色。
            cb->resourceUpdate(updates);
            updates = nullptr;
            composite(cb, finalTarget ? finalTarget : target.rt, originalActor);
            return;
        }
        ++generation;
        activeTextureKeys.clear();
        activeTextureKeys.insert(item["key"].toString("empty"));
        for (auto it = musicTextures.begin(); it != musicTextures.end(); ++it) {
            activeTextureKeys.insert(it.key());
        }
        // 恢复图形设备时先还原仍可见的旧唱片，最后绑定当前曲目。
        for (auto it = musicTextures.begin(); it != musicTextures.end(); ++it) {
            if (it.key() != item["key"].toString("empty") && !labels.contains(it.key())) {
                updateMusic(it->item, it->cover);
            }
        }
        updateMusic(item, cover);
        auto draws = musicDraws(frame);
        // 即使被原版唱片覆盖，也保留烘焙唱片资源，归位时不再重建材质绑定和实例缓冲。
        auto data = prepare(draws);
        for (auto it = materials.begin(); it != materials.end();) {
            if (it->used != generation) {
                retire(it->bindings);
                retire(it->uniform.buffer);
                it = materials.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = instances.begin(); it != instances.end();) {
            if (it->used != generation) {
                retire(it->buffer);
                it = instances.erase(it);
            } else {
                ++it;
            }
        }
        QList<Draw> opaque;
        QList<Draw> transparent;
        for (const auto &draw : data) {
            if (draw.data["shadowOnly"].toBool()) {
                continue;
            }
            if (draw.data["transparent"].toBool()) {
                transparent.append(draw);
            } else {
                opaque.append(draw);
            }
        }
        const auto sort = [](auto &draws) {
            std::stable_sort(draws.begin(), draws.end(), [](const auto &first, const auto &second) {
                return first.data["order"].toInt() < second.data["order"].toInt();
            });
        };
        sort(opaque);
        sort(transparent);
        if (frame["batchDraws"].toBool(true)) {
            opaque = batchDraws(opaque);
        }
        for (auto it = batchInstances.begin(); it != batchInstances.end();) {
            if (it->used != generation) {
                retire(it->buffer);
                it = batchInstances.erase(it);
            } else {
                ++it;
            }
        }
        if (g.opticalLight.w() >= 1.f) {
            // 先准备资源再省略被覆盖的绘制，既不重复消耗 GPU，也不在归位时重新分配。
            const auto hideSelected = [](auto &draws) {
                draws.erase(
                    std::remove_if(draws.begin(), draws.end(),
                                   [](const auto &draw) { return draw.data["musicSelected"].toBool(); }),
                    draws.end());
            };
            hideSelected(opaque);
            hideSelected(transparent);
        }
        cb->resourceUpdate(updates);
        updates = nullptr;
        const auto background = vector(frame["background"]);
        cb->beginPass(color.rt, QColor::fromRgbF(background.x(), background.y(), background.z()), {1, 0});
        geometry(cb, opaque);
        geometry(cb, transparent);
        cb->endPass();
        cachedGeometryRevision = geometryRevision;
        cachedItem = item;
        cachedCoverRevision = cover.cacheKey();
        cachedLightingEnabled = frame["opticalLightEnabled"].toBool(true);
        cachedSelectedHidden = g.opticalLight.w() >= 1.f;
        composite(cb, finalTarget ? finalTarget : target.rt, originalActor);
    }
};
class ItemRenderer : public QQuickRhiItemRenderer {
    Engine *backend = nullptr;
    std::unique_ptr<Renderer> renderer;
    QJsonObject frame, item;
    SceneDrawBuffers buffers;
    Engine::MusicTextures musicTextures;
    QImage cover;
    QString root;
    QQuickItem *originalSource = nullptr;

  public:
    ~ItemRenderer() override {
        if (backend) {
            backend->recordOpticalRelease();
        }
    }
    void synchronize(QQuickRhiItem *value) override {
        auto scene = static_cast<OpticalScene *>(value);
        auto engine = scene->engine();
        backend = engine;
        originalSource = scene->originalSource();
        if (engine) {
            frame = engine->frame();
            frame["opticalLightEnabled"] = scene->lightingEnabled();
            frame["geometryRevision"] = double(engine->geometryRevision());
            buffers = engine->drawBuffers();
            musicTextures = engine->musicTextures();
            item = engine->current();
            cover = engine->cover();
            root = engine->root();
        }
    }
    void initialize(QRhiCommandBuffer *) override {
        if (renderer && renderer->rhi != rhi()) {
            renderer.reset();
        }
    }
    void render(QRhiCommandBuffer *cb) override {
        if (frame.isEmpty() || (frame["mode"] == "boot" && frame["time"].toDouble() < 21.9)) {
            cb->beginPass(renderTarget(), Qt::transparent, {1, 0});
            cb->endPass();
            return;
        }
        if (!renderer) {
            renderer = std::make_unique<Renderer>(root, rhi());
        }
        renderer->nativeBuffers = buffers;
        const auto previousBindings = renderer->bindingCreations;
        const auto previousUploads = renderer->coverUploads;
        const auto previousDrawCalls = renderer->drawCalls;
        QRhiTexture *originalActor = nullptr;
        if (frame["originalActorBlend"].toDouble() > 0 && originalSource &&
            originalSource->isTextureProvider()) {
            const auto provider = originalSource->textureProvider();
            if (provider && provider->texture()) {
                originalActor = provider->texture()->rhiTexture();
            }
        }
        renderer->render(cb, frame, renderTarget()->pixelSize(), item, cover, renderTarget(), musicTextures,
                         originalActor);
        if (backend) {
            backend->recordRender(
                renderer->resources.size() + renderer->sized.size() + renderer->transient.size(),
                renderer->bindingCreations - previousBindings, renderer->musicTextureBytes(),
                renderer->coverUploads - previousUploads, renderer->drawCalls - previousDrawCalls, true);
        }
    }
};
} // namespace OpticalRendering

OpticalScene::OpticalScene(QQuickItem *parent) : QQuickRhiItem(parent) {
    setAlphaBlending(true);
}
void OpticalScene::setEngine(Engine *value) {
    if (value == m_engine) {
        return;
    }
    if (m_engine) {
        disconnect(m_engine, nullptr, this, nullptr);
    }
    m_engine = value;
    if (value) {
        connect(value, &Engine::opticalSceneUpdated, this, [this] { update(); });
    }
    emit engineChanged();
    update();
}
QQuickRhiItemRenderer *OpticalScene::createRenderer() {
    return new OpticalRendering::ItemRenderer;
}
void OpticalScene::setLightingEnabled(bool enabled) {
    if (m_lightingEnabled == enabled) {
        return;
    }
    m_lightingEnabled = enabled;
    emit lightingEnabledChanged();
    update();
}

void OpticalScene::setOriginalSource(QQuickItem *source) {
    if (m_originalSource == source) {
        return;
    }
    m_originalSource = source;
    emit originalSourceChanged();
    update();
}
