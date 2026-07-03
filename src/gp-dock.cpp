/*
GamePulse for OBS — dock implementation.
*/

#include "gp-dock.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

#include <obs-module.h>
#include <obs-frontend-api.h>

#include "gp-core.h"
#include "gp-twitch.h"
#include "gp-types.h"

namespace gamepulse {

GpDock::GpDock(QWidget *parent) : QWidget(parent)
{
	buildUi();

	GpCore::instance().on_ui_update = [this](const GpEvent *ev) {
		/* Called on the UI thread by the core; forward safely even if a
		   stray call arrives off-thread. */
		if (ev) {
			QString time = QString::fromStdString(ev->stream_ms >= 0   ? format_clock(ev->stream_ms)
							      : ev->record_ms >= 0 ? format_clock(ev->record_ms)
										   : "--:--");
			QString actions;
			auto add = [&](const char *s) {
				if (!actions.isEmpty())
					actions += "+";
				actions += s;
			};
			if (ev->actions_taken & ACTION_CHAPTER)
				add("chapter");
			if (ev->actions_taken & ACTION_CLIP)
				add("clip");
			if (ev->actions_taken & ACTION_MARKER)
				add("marker");
			QMetaObject::invokeMethod(this, "appendEventRow", Qt::QueuedConnection, Q_ARG(QString, time),
						  Q_ARG(QString, QString::fromStdString(ev->label)),
						  Q_ARG(QString, QString::fromStdString(ev->detail)),
						  Q_ARG(int, ev->importance), Q_ARG(QString, actions));
		}
		QMetaObject::invokeMethod(this, "refreshStatus", Qt::QueuedConnection);
	};

	refreshStatus();
}

GpDock::~GpDock()
{
	GpCore::instance().on_ui_update = nullptr;
}

void GpDock::buildUi()
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(8, 8, 8, 8);
	root->setSpacing(8);

	/* ---- status row ---- */
	auto *statusRow = new QHBoxLayout();
	status_dot_ = new QLabel(QString::fromUtf8("\xE2\x97\x8F"));
	status_dot_->setStyleSheet("color:#888;");
	status_text_ = new QLabel(obs_module_text("Dock.ServerStopped"));
	game_label_ = new QLabel("");
	game_label_->setStyleSheet("color:#8ab4f8;");
	statusRow->addWidget(status_dot_);
	statusRow->addWidget(status_text_, 1);
	statusRow->addWidget(game_label_);
	root->addLayout(statusRow);

	/* ---- quick actions ---- */
	auto *actions = new QGroupBox(obs_module_text("Dock.QuickActions"));
	auto *av = new QVBoxLayout(actions);

	auto *btnRow = new QHBoxLayout();
	auto *bookmarkBtn = new QPushButton(obs_module_text("Dock.Bookmark"));
	auto *clipBtn = new QPushButton(obs_module_text("Dock.Clip"));
	auto *exportBtn = new QPushButton(obs_module_text("Dock.Export"));
	connect(bookmarkBtn, &QPushButton::clicked, this, &GpDock::onBookmark);
	connect(clipBtn, &QPushButton::clicked, this, &GpDock::onClip);
	connect(exportBtn, &QPushButton::clicked, this, &GpDock::onExport);
	btnRow->addWidget(bookmarkBtn);
	btnRow->addWidget(clipBtn);
	btnRow->addWidget(exportBtn);
	av->addLayout(btnRow);

	auto *commentRow = new QHBoxLayout();
	comment_edit_ = new QLineEdit();
	comment_edit_->setPlaceholderText(obs_module_text("Dock.CommentPlaceholder"));
	auto *commentBtn = new QPushButton(obs_module_text("Dock.AddComment"));
	connect(commentBtn, &QPushButton::clicked, this, &GpDock::onComment);
	connect(comment_edit_, &QLineEdit::returnPressed, this, &GpDock::onComment);
	commentRow->addWidget(comment_edit_, 1);
	commentRow->addWidget(commentBtn);
	av->addLayout(commentRow);
	root->addWidget(actions);

	/* ---- server ---- */
	auto *server = new QGroupBox(obs_module_text("Dock.Server"));
	auto *sv = new QVBoxLayout(server);
	auto *portRow = new QHBoxLayout();
	portRow->addWidget(new QLabel(obs_module_text("Dock.Port")));
	port_spin_ = new QSpinBox();
	port_spin_->setRange(1, 65535);
	port_spin_->setValue((int)obs_data_get_int(GpCore::instance().config(), "port"));
	portRow->addWidget(port_spin_);
	portRow->addWidget(new QLabel(obs_module_text("Dock.Token")));
	token_edit_ = new QLineEdit(obs_data_get_string(GpCore::instance().config(), "token"));
	token_edit_->setPlaceholderText(obs_module_text("Dock.TokenPlaceholder"));
	portRow->addWidget(token_edit_, 1);
	sv->addLayout(portRow);

	auto *serverBtnRow = new QHBoxLayout();
	server_btn_ = new QPushButton(obs_module_text("Dock.StopServer"));
	connect(server_btn_, &QPushButton::clicked, this, &GpDock::onToggleServer);
	auto *applyBtn = new QPushButton(obs_module_text("Dock.ApplyRestart"));
	connect(applyBtn, &QPushButton::clicked, this, &GpDock::onApplyServer);
	serverBtnRow->addWidget(server_btn_);
	serverBtnRow->addWidget(applyBtn);
	sv->addLayout(serverBtnRow);
	root->addWidget(server);

	/* ---- twitch ---- */
	auto *twitch = new QGroupBox(obs_module_text("Dock.Twitch"));
	auto *tv = new QVBoxLayout(twitch);
	twitch_label_ = new QLabel(obs_module_text("Dock.TwitchNotConnected"));
	tv->addWidget(twitch_label_);

	auto *cidRow = new QHBoxLayout();
	cidRow->addWidget(new QLabel(obs_module_text("Dock.ClientId")));
	client_id_edit_ = new QLineEdit(GpCore::instance().twitch()->client_id().c_str());
	client_id_edit_->setPlaceholderText(obs_module_text("Dock.ClientIdPlaceholder"));
	cidRow->addWidget(client_id_edit_, 1);
	tv->addLayout(cidRow);

	auto *twBtnRow = new QHBoxLayout();
	twitch_btn_ = new QPushButton(obs_module_text("Dock.TwitchConnect"));
	connect(twitch_btn_, &QPushButton::clicked, this, &GpDock::onTwitchConnect);
	auto *logoutBtn = new QPushButton(obs_module_text("Dock.TwitchLogout"));
	connect(logoutBtn, &QPushButton::clicked, this, &GpDock::onTwitchLogout);
	twBtnRow->addWidget(twitch_btn_);
	twBtnRow->addWidget(logoutBtn);
	tv->addLayout(twBtnRow);

	auto *chatRow = new QHBoxLayout();
	chat_check_ = new QCheckBox(obs_module_text("Dock.ChatClip"));
	chat_check_->setChecked(GpCore::instance().twitch()->chat_config().enabled);
	connect(chat_check_, &QCheckBox::toggled, this, &GpDock::onToggleChat);
	chat_perm_ = new QComboBox();
	chat_perm_->addItems({"anyone", "sub", "vip", "mod", "broadcaster"});
	chat_perm_->setCurrentText(GpCore::instance().twitch()->chat_config().permission.c_str());
	connect(chat_perm_, &QComboBox::currentTextChanged, this, &GpDock::onToggleChat);
	chatRow->addWidget(chat_check_);
	chatRow->addWidget(new QLabel(obs_module_text("Dock.ChatPermission")));
	chatRow->addWidget(chat_perm_, 1);
	tv->addLayout(chatRow);
	root->addWidget(twitch);

	/* ---- event log ---- */
	auto *logBox = new QGroupBox(obs_module_text("Dock.EventLog"));
	auto *lv = new QVBoxLayout(logBox);
	event_list_ = new QListWidget();
	event_list_->setAlternatingRowColors(true);
	lv->addWidget(event_list_, 1);
	summary_label_ = new QLabel("");
	summary_label_->setStyleSheet("color:#9aa;");
	lv->addWidget(summary_label_);
	root->addWidget(logBox, 1);
}

void GpDock::refreshStatus()
{
	CoreStatus s = GpCore::instance().status();

	if (s.server_running) {
		status_dot_->setStyleSheet(s.clients > 0 ? "color:#2ecc71;" : "color:#f1c40f;");
		QString txt = QString(obs_module_text("Dock.ServerRunning")).arg(s.clients);
		status_text_->setText(txt);
		server_btn_->setText(obs_module_text("Dock.StopServer"));
	} else {
		status_dot_->setStyleSheet("color:#888;");
		status_text_->setText(obs_module_text("Dock.ServerStopped"));
		server_btn_->setText(obs_module_text("Dock.StartServer"));
	}

	if (!s.game_name.empty()) {
		QString g = QString::fromStdString(s.game_name);
		if (s.recording)
			g += " \xE2\x8F\xBA";
		if (s.streaming)
			g += " \xF0\x9F\x94\xB4";
		game_label_->setText(g);
	} else {
		game_label_->setText(s.recording || s.streaming ? (s.streaming ? "LIVE" : "REC") : "");
	}

	if (s.twitch_authed) {
		QString t =
			QString(obs_module_text("Dock.TwitchConnectedAs")).arg(QString::fromStdString(s.twitch_login));
		if (s.chat_listener)
			t += QString::fromUtf8("  \xF0\x9F\x92\xAC");
		twitch_label_->setText(t);
		twitch_btn_->setText(obs_module_text("Dock.TwitchReconnect"));
	} else {
		twitch_label_->setText(obs_module_text("Dock.TwitchNotConnected"));
		twitch_btn_->setText(obs_module_text("Dock.TwitchConnect"));
	}

	summary_label_->setText(QString::fromStdString(GpCore::instance().journal().summary()));
}

void GpDock::appendEventRow(const QString &time, const QString &label, const QString &detail, int importance,
			    const QString &actions)
{
	QString text = QString("[%1]  %2").arg(time, label);
	if (!detail.isEmpty())
		text += "  \xE2\x80\x94  " + detail;
	if (!actions.isEmpty())
		text += "   (" + actions + ")";

	auto *item = new QListWidgetItem(text);
	QColor c;
	switch (importance) {
	case IMP_EPIC:
		c = QColor(255, 120, 90);
		break;
	case IMP_NOTABLE:
		c = QColor(120, 230, 160);
		break;
	case IMP_MINOR:
		c = QColor(190, 200, 210);
		break;
	default:
		c = QColor(130, 138, 150);
		break;
	}
	item->setForeground(c);
	event_list_->insertItem(0, item);
	while (event_list_->count() > 200)
		delete event_list_->takeItem(event_list_->count() - 1);

	summary_label_->setText(QString::fromStdString(GpCore::instance().journal().summary()));
}

/* ---- action slots ---- */

void GpDock::onBookmark()
{
	GpCore::instance().submit_manual("manual_bookmark", "", ACTION_CHAPTER | ACTION_MARKER);
}

void GpDock::onComment()
{
	QString text = comment_edit_->text().trimmed();
	if (text.isEmpty())
		return;
	comment_edit_->clear();
	GpCore::instance().submit_manual("manual_comment", text.toStdString(), ACTION_CHAPTER | ACTION_MARKER);
}

void GpDock::onClip()
{
	GpCore::instance().submit_manual("manual_clip", "", ACTION_CLIP);
}

void GpDock::onExport()
{
	std::string dir = GpCore::instance().export_now();
	if (!dir.empty()) {
		QMessageBox::information(this, obs_module_text("Dock.Export"),
					 QString(obs_module_text("Dock.ExportedTo")).arg(QString::fromStdString(dir)));
		QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(dir)));
	}
}

void GpDock::onToggleServer()
{
	CoreStatus s = GpCore::instance().status();
	obs_data_set_bool(GpCore::instance().config(), "server_enabled", !s.server_running);
	if (s.server_running)
		GpCore::instance().stop_server();
	else
		GpCore::instance().restart_server();
	GpCore::instance().save_config();
	refreshStatus();
}

void GpDock::onApplyServer()
{
	obs_data_t *cfg = GpCore::instance().config();
	obs_data_set_int(cfg, "port", port_spin_->value());
	obs_data_set_string(cfg, "token", token_edit_->text().toUtf8().constData());
	obs_data_set_bool(cfg, "server_enabled", true);
	bool ok = GpCore::instance().restart_server();
	GpCore::instance().save_config();
	if (!ok)
		QMessageBox::warning(this, obs_module_text("Dock.Server"), obs_module_text("Dock.PortInUse"));
	refreshStatus();
}

void GpDock::onTwitchConnect()
{
	TwitchService *tw = GpCore::instance().twitch();
	std::string cid = client_id_edit_->text().trimmed().toStdString();
	if (cid.empty()) {
		QMessageBox::warning(this, obs_module_text("Dock.Twitch"), obs_module_text("Dock.NeedClientId"));
		return;
	}
	tw->set_client_id(cid);
	GpCore::instance().save_config();

	twitch_btn_->setEnabled(false);
	twitch_btn_->setText(obs_module_text("Dock.TwitchWaiting"));

	tw->begin_device_auth(
		[this](std::string user_code, std::string verification_uri) {
			QString code = QString::fromStdString(user_code);
			QString uri = QString::fromStdString(verification_uri);
			QMetaObject::invokeMethod(
				this,
				[this, code, uri]() {
					QDesktopServices::openUrl(QUrl(uri));
					QMessageBox::information(
						this, obs_module_text("Dock.TwitchConnect"),
						QString(obs_module_text("Dock.TwitchCodePrompt")).arg(code, uri));
				},
				Qt::QueuedConnection);
		},
		[this](bool ok, std::string message) {
			QString msg = QString::fromStdString(message);
			QMetaObject::invokeMethod(
				this,
				[this, ok, msg]() {
					twitch_btn_->setEnabled(true);
					if (!ok)
						QMessageBox::warning(this, obs_module_text("Dock.TwitchConnect"), msg);
					GpCore::instance().save_config();
					refreshStatus();
				},
				Qt::QueuedConnection);
		});
}

void GpDock::onTwitchLogout()
{
	GpCore::instance().twitch()->logout();
	GpCore::instance().save_config();
	refreshStatus();
}

void GpDock::onToggleChat()
{
	TwitchService *tw = GpCore::instance().twitch();
	TwitchService::ChatConfig cfg = tw->chat_config();
	cfg.enabled = chat_check_->isChecked();
	cfg.permission = chat_perm_->currentText().toStdString();
	tw->set_chat_config(cfg);
	tw->apply_chat_state();
	GpCore::instance().save_config();
	refreshStatus();
}

} // namespace gamepulse
