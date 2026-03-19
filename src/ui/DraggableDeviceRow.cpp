#include "ui/DraggableDeviceRow.h"

#include <utility>

#include <QApplication>
#include <QDataStream>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QStyle>

namespace battery_monitor {

namespace {

constexpr const char* kDeviceRowMimeType = "application/x-chargeview-device-row";

}  // namespace

DraggableDeviceRow::DraggableDeviceRow(std::string device_id, bool is_connected, QWidget* parent)
    : QFrame(parent), device_id_(std::move(device_id)), is_connected_(is_connected) {
    setAcceptDrops(true);
    setCursor(Qt::OpenHandCursor);
    setProperty("dragOver", false);
}

void DraggableDeviceRow::SetReorderCallback(ReorderCallback callback) {
    reorder_callback_ = std::move(callback);
}

void DraggableDeviceRow::SetDragStateCallback(DragStateCallback callback) {
    drag_state_callback_ = std::move(callback);
}

void DraggableDeviceRow::mousePressEvent(QMouseEvent* event) {
    if (event != nullptr && event->button() == Qt::LeftButton) {
        drag_start_pos_ = event->position().toPoint();
        setCursor(Qt::ClosedHandCursor);
    }
    QFrame::mousePressEvent(event);
}

void DraggableDeviceRow::mouseMoveEvent(QMouseEvent* event) {
    if (event == nullptr || !(event->buttons() & Qt::LeftButton)) {
        QFrame::mouseMoveEvent(event);
        return;
    }
    if ((event->position().toPoint() - drag_start_pos_).manhattanLength() < QApplication::startDragDistance()) {
        QFrame::mouseMoveEvent(event);
        return;
    }

    StartDrag();
    event->accept();
}

void DraggableDeviceRow::mouseReleaseEvent(QMouseEvent* event) {
    setCursor(Qt::OpenHandCursor);
    QFrame::mouseReleaseEvent(event);
}

void DraggableDeviceRow::dragEnterEvent(QDragEnterEvent* event) {
    if (CanAccept(event != nullptr ? event->mimeData() : nullptr)) {
        event->acceptProposedAction();
        SetDragOver(true);
        return;
    }
    if (event != nullptr) {
        event->ignore();
    }
}

void DraggableDeviceRow::dragMoveEvent(QDragMoveEvent* event) {
    if (CanAccept(event != nullptr ? event->mimeData() : nullptr)) {
        event->acceptProposedAction();
        return;
    }
    if (event != nullptr) {
        event->ignore();
    }
}

void DraggableDeviceRow::dragLeaveEvent(QDragLeaveEvent* event) {
    Q_UNUSED(event);
    SetDragOver(false);
}

void DraggableDeviceRow::dropEvent(QDropEvent* event) {
    std::string dragged_device_id;
    bool dragged_connected = false;
    if (event == nullptr ||
        !DecodePayload(event->mimeData(), &dragged_device_id, &dragged_connected) ||
        dragged_device_id == device_id_ || dragged_connected != is_connected_) {
        if (event != nullptr) {
            event->ignore();
        }
        SetDragOver(false);
        return;
    }

    SetDragOver(false);
    const bool insert_before_target = event->position().y() < (static_cast<double>(height()) / 2.0);
    if (reorder_callback_) {
        reorder_callback_(dragged_device_id, device_id_, is_connected_, insert_before_target);
    }
    event->acceptProposedAction();
}

QByteArray DraggableDeviceRow::BuildPayload(const std::string& device_id, bool is_connected) {
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream << QString::fromUtf8(device_id.c_str()) << is_connected;
    return payload;
}

bool DraggableDeviceRow::DecodePayload(const QMimeData* mime_data,
                                       std::string* device_id,
                                       bool* is_connected) {
    if (mime_data == nullptr || device_id == nullptr || is_connected == nullptr ||
        !mime_data->hasFormat(kDeviceRowMimeType)) {
        return false;
    }

    QByteArray payload = mime_data->data(kDeviceRowMimeType);
    QDataStream stream(&payload, QIODevice::ReadOnly);
    QString decoded_id;
    bool decoded_connected = false;
    stream >> decoded_id >> decoded_connected;
    if (stream.status() != QDataStream::Ok || decoded_id.isEmpty()) {
        return false;
    }

    *device_id = decoded_id.toUtf8().toStdString();
    *is_connected = decoded_connected;
    return true;
}

bool DraggableDeviceRow::CanAccept(const QMimeData* mime_data) const {
    std::string dragged_device_id;
    bool dragged_connected = false;
    return DecodePayload(mime_data, &dragged_device_id, &dragged_connected) &&
           dragged_connected == is_connected_ &&
           dragged_device_id != device_id_;
}

void DraggableDeviceRow::SetDragOver(bool enabled) {
    if (property("dragOver").toBool() == enabled) {
        return;
    }
    setProperty("dragOver", enabled);
    if (auto* widget_style = style(); widget_style != nullptr) {
        widget_style->unpolish(this);
        widget_style->polish(this);
    }
    update();
}

void DraggableDeviceRow::StartDrag() {
    auto* drag = new QDrag(this);
    auto* mime_data = new QMimeData();
    mime_data->setData(kDeviceRowMimeType, BuildPayload(device_id_, is_connected_));
    drag->setMimeData(mime_data);
    drag->setHotSpot(rect().center());
    if (drag_state_callback_) {
        drag_state_callback_(true);
    }
    drag->exec(Qt::MoveAction);
    if (drag_state_callback_) {
        drag_state_callback_(false);
    }
    setCursor(Qt::OpenHandCursor);
}

}  // namespace battery_monitor
