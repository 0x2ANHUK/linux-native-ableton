// Run offscreen against Qt 5; tests dispatch policy, not Live's session
// scroller.
#include "../shim/wheelhorizontal.cpp"
#include <QApplication>
#include <QWheelEvent>
#include <QWidget>
#include <cassert>
LiveBuild liveBuild() { return LiveBuild::Live12_4_7; }
class TQtView : public QWidget {
public:
  bool consumeVertical = false;
  int vertical = 0, horizontal = 0;
  void wheelEvent(QWheelEvent *event) override {
    if (event->angleDelta().y()) {
      ++vertical;
      event->setAccepted(consumeVertical);
    } else {
      ++horizontal;
      event->accept();
    }
  }
};
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  TQtView view;
  auto wheel = [&](Qt::KeyboardModifiers mods, QPoint delta) {
    QWheelEvent event(QPointF(1, 1), QPointF(1, 1), QPoint(), delta, Qt::NoButton, mods,
                      Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&view, &event);
  };
  wheel(Qt::ShiftModifier, QPoint(0, 120));
  assert(view.vertical == 0 && view.horizontal == 1);
  view.consumeVertical = true;
  wheel(Qt::ShiftModifier, QPoint(0, 120));
  assert(view.vertical == 0 && view.horizontal == 2);
  wheel(Qt::NoModifier, QPoint(0, 120));
  assert(view.vertical == 1 && view.horizontal == 2);
  wheel(Qt::ShiftModifier, QPoint(120, 0));
  assert(view.horizontal == 3);
}
