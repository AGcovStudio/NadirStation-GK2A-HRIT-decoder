#include "appinfo.h"
#include "mainwindow.h"

#include <QApplication>
#include <QFont>
#include <QColor>
#include <QIcon>
#include <QPalette>
#include <QStyleFactory>

#include <complex>
#include <vector>

namespace {

void applyTheme(QApplication& app) {
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QFont font(QStringLiteral("Microsoft YaHei UI"), 9);
    app.setFont(font);
    QPalette p;
    p.setColor(QPalette::Window, QColor(0xff, 0xff, 0xff));
    p.setColor(QPalette::WindowText, QColor(0x1f, 0x29, 0x33));
    p.setColor(QPalette::Base, QColor(0xff, 0xff, 0xff));
    p.setColor(QPalette::AlternateBase, QColor(0xf5, 0xf7, 0xfa));
    p.setColor(QPalette::Text, QColor(0x1f, 0x29, 0x33));
    p.setColor(QPalette::Button, QColor(0xf3, 0xf5, 0xf8));
    p.setColor(QPalette::ButtonText, QColor(0x1f, 0x29, 0x33));
    p.setColor(QPalette::ToolTipBase, QColor(0xff, 0xff, 0xe8));
    p.setColor(QPalette::ToolTipText, QColor(0x1f, 0x29, 0x33));
    p.setColor(QPalette::Highlight, QColor(0x1a, 0x5f, 0xc8));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::PlaceholderText, QColor(0x8a, 0x95, 0xa3));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x9a, 0xa4, 0xb0));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x9a, 0xa4, 0xb0));
    app.setPalette(p);
    // Square-ish, flat, one accent colour: a measurement tool, not a toy.
    app.setStyleSheet(QStringLiteral(R"(
        QMainWindow, QDialog { background:#ffffff; }
        QGroupBox { border:1px solid #d3d9e0; border-radius:2px; margin-top:14px; padding:10px 8px 8px 8px; background:#ffffff; }
        QGroupBox::title { subcontrol-origin:margin; left:8px; padding:0 4px; color:#33506e; font-weight:600; }
        QLineEdit, QDoubleSpinBox { background:#ffffff; border:1px solid #c3cbd5; border-radius:2px; padding:3px 6px; selection-background-color:#1a5fc8; }
        QLineEdit:focus, QDoubleSpinBox:focus { border-color:#1a5fc8; }
        QLineEdit:disabled, QDoubleSpinBox:disabled { background:#f3f5f8; color:#9aa4b0; }
        QPushButton { background:#f3f5f8; border:1px solid #c3cbd5; border-radius:2px; padding:4px 12px; }
        QPushButton:hover { background:#e8edf3; }
        QPushButton:pressed { background:#dde4ec; }
        QPushButton:disabled { color:#9aa4b0; background:#f7f8fa; }
        QPushButton#Primary { background:#1a5fc8; border:1px solid #154c9f; color:#ffffff; font-weight:600; }
        QPushButton#Primary:hover { background:#1f6ad9; }
        QPushButton#Primary:disabled { background:#9db9e3; border-color:#9db9e3; color:#ffffff; }
        QPushButton#Link { border:none; background:transparent; color:#5b6b7d; padding:0 8px; font-size:11px; }
        QPushButton#Link:hover { color:#1a5fc8; text-decoration:underline; }
        QCheckBox { spacing:6px; }
        QLabel#AppTitle { font-size:18px; font-weight:700; color:#14325c; }
        QLabel#Muted { color:#6b7785; font-size:11px; }
        QLabel#Value { font-size:12px; font-weight:600; color:#14325c; }
        QLabel#Credit { color:#7b8795; font-size:11px; }
        QLabel#Preview { background:#f3f5f8; border:1px solid #c3cbd5; border-radius:0; color:#8a95a3; }
        QProgressBar { background:#e6eaf0; border:none; border-radius:0; }
        QProgressBar::chunk { background:#1a5fc8; border-radius:0; }
        QListWidget, QPlainTextEdit, QTextBrowser { background:#ffffff; border:1px solid #d3d9e0; border-radius:2px; color:#1f2933; }
        QListWidget::item:selected { background:#dbe7f8; color:#14325c; }
        QPlainTextEdit { font-family:Consolas,"Cascadia Mono",monospace; font-size:11px; color:#33404d; background:#f9fafc; }
        QStatusBar { background:#f3f5f8; border-top:1px solid #d3d9e0; color:#5b6b7d; }
        QStatusBar::item { border:none; }
        QToolTip { background:#ffffe8; color:#1f2933; border:1px solid #c3cbd5; }
    )"));
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AGcov"));
    QCoreApplication::setApplicationName(QStringLiteral("NadirStation_GK2A-HRIT_decoder"));
    QCoreApplication::setApplicationVersion(QString::fromUtf8(appinfo::kVersion));
    qRegisterMetaType<std::vector<std::complex<float>>>("std::vector<std::complex<float> >");
    qRegisterMetaType<std::vector<std::complex<float>>>("std::vector<std::complex<float>>");
    qRegisterMetaType<hritapp::PipelineStats>("hritapp::PipelineStats");
    applyTheme(app);
    app.setWindowIcon(QIcon(QStringLiteral(":/icon.png")));

    MainWindow window;
    window.show();
    return app.exec();
}
