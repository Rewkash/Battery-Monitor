#pragma once

#include <functional>
#include <string>

#include <QFrame>

class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QMimeData;
class QMouseEvent;

namespace battery_monitor {

class DraggableDeviceRow final : public QFrame {
   public:
    using ReorderCallback = std::function<void(const std::string& dragged_device_id,
                                               const std::string& target_device_id,
                                               bool connected_queue,
                                               bool insert_before_target)>;
    using DragStateCallback = std::function<void(bool active)>;

    DraggableDeviceRow(std::string device_id, bool is_connected, QWidget* parent = nullptr);

    void SetReorderCallback(ReorderCallback callback);
    void SetDragStateCallback(DragStateCallback callback);

   protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

   private:
    static QByteArray BuildPayload(const std::string& device_id, bool is_connected);
    static bool DecodePayload(const QMimeData* mime_data, std::string* device_id, bool* is_connected);
    bool CanAccept(const QMimeData* mime_data) const;
    void SetDragOver(bool enabled);
    void StartDrag();

    std::string device_id_;
    bool is_connected_ = false;
    QPoint drag_start_pos_;
    ReorderCallback reorder_callback_;
    DragStateCallback drag_state_callback_;
};

}  // namespace battery_monitor
