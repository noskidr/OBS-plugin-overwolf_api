/*
GamePulse for OBS — "GamePulse Overlay" source.

Renders the recent event feed + session stats into a QImage (CPU raster,
safe on the graphics thread) and uploads it as a GS_BGRA texture. Repaints
only when the feed changes or items expire; cheap when idle.
*/

#include <obs-module.h>
#include <util/platform.h>

#include <QColor>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QString>

#include "gp-core.h"
#include "plugin-support.h"

using namespace gamepulse;

namespace {

struct OverlaySource {
	obs_source_t *source = nullptr;
	uint32_t width = 460;
	uint32_t height = 340;
	bool show_feed = true;
	bool show_stats = true;
	double font_scale = 1.0;

	QImage image;
	gs_texture_t *texture = nullptr;
	uint32_t tex_w = 0, tex_h = 0;

	uint64_t painted_signature = 0;
	bool need_upload = false;
};

QColor importance_color(int importance)
{
	switch (importance) {
	case IMP_EPIC:
		return QColor(255, 92, 56);
	case IMP_NOTABLE:
		return QColor(84, 255, 158);
	case IMP_MINOR:
		return QColor(160, 176, 196);
	default:
		return QColor(110, 120, 134);
	}
}

/* signature that changes whenever the visible state changes */
uint64_t feed_signature(OverlayFeed &feed, uint64_t now_ns, int *visible_out)
{
	std::lock_guard<std::mutex> lock(feed.mutex);
	uint64_t sig = feed.version * 1000003ULL;
	int visible = 0;
	for (const OverlayItem &it : feed.items) {
		if (it.expires_ns > now_ns) {
			visible++;
			/* quantize remaining time to 100ms so fade animates */
			uint64_t remain = (it.expires_ns - now_ns) / 100000000ULL;
			if (remain < 8)
				sig = sig * 31 + remain;
		}
	}
	sig = sig * 131 + (uint64_t)visible;
	sig = sig * 131 + (uint64_t)(feed.kills * 73 + feed.deaths * 37 + feed.assists * 17 + feed.clips * 7 +
				     feed.chapters * 3 + feed.markers);
	*visible_out = visible;
	return sig;
}

void paint_overlay(OverlaySource *s, uint64_t now_ns)
{
	if (s->image.width() != (int)s->width || s->image.height() != (int)s->height) {
		s->image = QImage((int)s->width, (int)s->height, QImage::Format_ARGB32_Premultiplied);
	}
	s->image.fill(Qt::transparent);

	OverlayFeed &feed = GpCore::instance().overlay_feed();

	QPainter p(&s->image);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setRenderHint(QPainter::TextAntialiasing, true);

	const double fs = s->font_scale;
	QFont label_font("Segoe UI", (int)(13 * fs), QFont::Bold);
	QFont detail_font("Segoe UI", (int)(10 * fs));
	QFont stats_font("Segoe UI", (int)(11 * fs), QFont::DemiBold);

	int y = 8;
	const int card_h = (int)(46 * fs);
	const int gap = 6;

	std::vector<OverlayItem> items;
	int kills, deaths, assists, clips;
	std::string game;
	{
		std::lock_guard<std::mutex> lock(feed.mutex);
		for (const OverlayItem &it : feed.items)
			if (it.expires_ns > now_ns)
				items.push_back(it);
		kills = feed.kills;
		deaths = feed.deaths;
		assists = feed.assists;
		clips = feed.clips;
		game = feed.game_name;
	}

	if (s->show_feed) {
		/* newest first */
		for (auto it = items.rbegin(); it != items.rend(); ++it) {
			if (y + card_h > (int)s->height - (s->show_stats ? (int)(34 * fs) : 0))
				break;

			double alpha = 1.0;
			uint64_t remain_ns = it->expires_ns - now_ns;
			if (remain_ns < 600000000ULL)
				alpha = (double)remain_ns / 600000000.0;

			p.setOpacity(alpha);

			QRectF card(6, y, s->width - 12, card_h);
			QPainterPath path;
			path.addRoundedRect(card, 8, 8);
			p.fillPath(path, QColor(16, 18, 24, 216));

			QColor accent = importance_color(it->importance);
			p.fillRect(QRectF(card.left(), card.top() + 6, 4, card.height() - 12), accent);

			p.setPen(QColor(245, 247, 250));
			p.setFont(label_font);
			QRectF label_rect(card.left() + 14, card.top() + 4, card.width() - 20, card.height() / 2);
			p.drawText(label_rect, Qt::AlignLeft | Qt::AlignVCenter, QString::fromUtf8(it->label.c_str()));

			if (!it->detail.empty()) {
				p.setPen(QColor(168, 178, 191));
				p.setFont(detail_font);
				QRectF detail_rect(card.left() + 14, card.top() + card.height() / 2 - 2,
						   card.width() - 20, card.height() / 2 - 4);
				QString detail = QString::fromUtf8(it->detail.c_str());
				detail = p.fontMetrics().elidedText(detail, Qt::ElideRight, (int)detail_rect.width());
				p.drawText(detail_rect, Qt::AlignLeft | Qt::AlignVCenter, detail);
			}

			y += card_h + gap;
		}
		p.setOpacity(1.0);
	}

	if (s->show_stats) {
		QRectF bar(6, s->height - (int)(30 * fs) - 6, s->width - 12, (int)(30 * fs));
		QPainterPath path;
		path.addRoundedRect(bar, 8, 8);
		p.fillPath(path, QColor(16, 18, 24, 216));

		QString stats = QString("K %1   D %2   A %3   \xF0\x9F\x8E\xAC %4")
					.arg(kills)
					.arg(deaths)
					.arg(assists)
					.arg(clips);
		p.setPen(QColor(235, 240, 245));
		p.setFont(stats_font);
		p.drawText(bar.adjusted(12, 0, -12, 0), Qt::AlignLeft | Qt::AlignVCenter, stats);

		if (!game.empty()) {
			p.setPen(QColor(140, 152, 166));
			p.drawText(bar.adjusted(12, 0, -12, 0), Qt::AlignRight | Qt::AlignVCenter,
				   QString::fromUtf8(game.c_str()));
		}
	}

	p.end();
	s->need_upload = true;
}

const char *overlay_get_name(void *)
{
	return obs_module_text("OverlaySource.Name");
}

void overlay_update(void *data, obs_data_t *settings)
{
	OverlaySource *s = static_cast<OverlaySource *>(data);
	s->width = (uint32_t)obs_data_get_int(settings, "width");
	s->height = (uint32_t)obs_data_get_int(settings, "height");
	s->show_feed = obs_data_get_bool(settings, "show_feed");
	s->show_stats = obs_data_get_bool(settings, "show_stats");
	s->font_scale = obs_data_get_double(settings, "font_scale");
	if (s->width < 120)
		s->width = 120;
	if (s->height < 80)
		s->height = 80;
	s->painted_signature = 0; /* force repaint */
}

void *overlay_create(obs_data_t *settings, obs_source_t *source)
{
	OverlaySource *s = new OverlaySource;
	s->source = source;
	overlay_update(s, settings);
	return s;
}

void overlay_destroy(void *data)
{
	OverlaySource *s = static_cast<OverlaySource *>(data);
	if (s->texture) {
		obs_enter_graphics();
		gs_texture_destroy(s->texture);
		obs_leave_graphics();
	}
	delete s;
}

uint32_t overlay_width(void *data)
{
	return static_cast<OverlaySource *>(data)->width;
}

uint32_t overlay_height(void *data)
{
	return static_cast<OverlaySource *>(data)->height;
}

void overlay_tick(void *data, float)
{
	OverlaySource *s = static_cast<OverlaySource *>(data);
	uint64_t now_ns = os_gettime_ns();

	int visible = 0;
	uint64_t sig = feed_signature(GpCore::instance().overlay_feed(), now_ns, &visible);
	sig = sig * 7 + (uint64_t)(s->width * 31 + s->height);

	if (sig != s->painted_signature) {
		s->painted_signature = sig;
		paint_overlay(s, now_ns);
	}
}

void overlay_render(void *data, gs_effect_t *)
{
	OverlaySource *s = static_cast<OverlaySource *>(data);

	if (s->need_upload && !s->image.isNull()) {
		s->need_upload = false;
		if (!s->texture || s->tex_w != (uint32_t)s->image.width() || s->tex_h != (uint32_t)s->image.height()) {
			if (s->texture)
				gs_texture_destroy(s->texture);
			const uint8_t *bits = s->image.constBits();
			s->texture = gs_texture_create((uint32_t)s->image.width(), (uint32_t)s->image.height(), GS_BGRA,
						       1, &bits, GS_DYNAMIC);
			s->tex_w = (uint32_t)s->image.width();
			s->tex_h = (uint32_t)s->image.height();
		} else {
			gs_texture_set_image(s->texture, s->image.constBits(), (uint32_t)s->image.bytesPerLine(),
					     false);
		}
	}

	if (!s->texture)
		return;

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_technique_t *tech = gs_effect_get_technique(effect, "Draw");

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA); /* premultiplied alpha */

	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), s->texture);
	gs_draw_sprite(s->texture, 0, s->tex_w, s->tex_h);
	gs_technique_end_pass(tech);
	gs_technique_end(tech);

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous);
}

obs_properties_t *overlay_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_int(props, "width", obs_module_text("OverlaySource.Width"), 120, 3840, 2);
	obs_properties_add_int(props, "height", obs_module_text("OverlaySource.Height"), 80, 2160, 2);
	obs_properties_add_bool(props, "show_feed", obs_module_text("OverlaySource.ShowFeed"));
	obs_properties_add_bool(props, "show_stats", obs_module_text("OverlaySource.ShowStats"));
	obs_properties_add_float_slider(props, "font_scale", obs_module_text("OverlaySource.FontScale"), 0.5, 3.0, 0.1);
	return props;
}

void overlay_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "width", 460);
	obs_data_set_default_int(settings, "height", 340);
	obs_data_set_default_bool(settings, "show_feed", true);
	obs_data_set_default_bool(settings, "show_stats", true);
	obs_data_set_default_double(settings, "font_scale", 1.0);
}

struct obs_source_info overlay_source_info = {};

} // namespace

extern "C" void gp_register_overlay_source(void)
{
	overlay_source_info.id = "gamepulse_overlay";
	overlay_source_info.type = OBS_SOURCE_TYPE_INPUT;
	overlay_source_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	overlay_source_info.get_name = overlay_get_name;
	overlay_source_info.create = overlay_create;
	overlay_source_info.destroy = overlay_destroy;
	overlay_source_info.update = overlay_update;
	overlay_source_info.get_defaults = overlay_defaults;
	overlay_source_info.get_properties = overlay_properties;
	overlay_source_info.get_width = overlay_width;
	overlay_source_info.get_height = overlay_height;
	overlay_source_info.video_tick = overlay_tick;
	overlay_source_info.video_render = overlay_render;
	overlay_source_info.icon_type = OBS_ICON_TYPE_GAME_CAPTURE;

	obs_register_source(&overlay_source_info);
}
