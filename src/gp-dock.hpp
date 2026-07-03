/*
GamePulse for OBS — control/event-log dock (Qt widget).

All slots run on the Qt main (UI) thread. GpCore marshals pipeline updates
here via QMetaObject::invokeMethod(..., Qt::QueuedConnection).
*/

#pragma once

#include <QWidget>

class QLabel;
class QListWidget;
class QPushButton;
class QLineEdit;
class QSpinBox;
class QCheckBox;
class QComboBox;

namespace gamepulse {

struct GpEvent;

class GpDock : public QWidget {
	Q_OBJECT

public:
	explicit GpDock(QWidget *parent = nullptr);
	~GpDock() override;

	/* Called (on UI thread) by core after each processed event / status change. */
	void notify(const GpEvent *ev);

public slots:
	void refreshStatus();

private slots:
	void onBookmark();
	void onComment();
	void onClip();
	void onExport();
	void onTestEvent();
	void onOpenSessions();
	void onToggleServer();
	void onApplyServer();
	void onAutomationChanged();
	void onTwitchConnect();
	void onTwitchLogout();
	void onToggleChat();
	void appendEventRow(const QString &time, const QString &label, const QString &detail, int importance,
			    const QString &actions);

private:
	void buildUi();
	void refreshSceneCombos();

	/* status */
	QLabel *status_dot_ = nullptr;
	QLabel *status_text_ = nullptr;
	QLabel *game_label_ = nullptr;
	QLabel *context_label_ = nullptr;
	QLabel *twitch_label_ = nullptr;

	/* automation */
	QCheckBox *auto_replay_check_ = nullptr;
	QCheckBox *auto_record_check_ = nullptr;
	QCheckBox *split_match_check_ = nullptr;
	QCheckBox *export_match_check_ = nullptr;
	QComboBox *scene_game_combo_ = nullptr;
	QComboBox *scene_lobby_combo_ = nullptr;
	QComboBox *scene_privacy_combo_ = nullptr;
	bool building_ui_ = false;

	/* controls */
	QLineEdit *comment_edit_ = nullptr;
	QPushButton *server_btn_ = nullptr;
	QSpinBox *port_spin_ = nullptr;
	QLineEdit *token_edit_ = nullptr;
	QLineEdit *client_id_edit_ = nullptr;
	QPushButton *twitch_btn_ = nullptr;
	QCheckBox *chat_check_ = nullptr;
	QComboBox *chat_perm_ = nullptr;

	QListWidget *event_list_ = nullptr;
	QLabel *summary_label_ = nullptr;
};

} // namespace gamepulse
