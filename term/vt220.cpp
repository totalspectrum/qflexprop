/*****************************************************************************
 *
 *  VT - Virtual Terminal emulation (VT220 and variants, i.e. ANSI)
 * Copyright © 2013-2021 Jürgen Buchmüller <pullmoll@t-online.de>
 *
 * See the file LICENSE for the details of the BSD-3-Clause terms.
 *
 *****************************************************************************/
#include <QFocusEvent>
#include <QFontDatabase>
#include "vtscrollarea.h"
#include "vt220.h"

#define	DEBUG_FONTINFO	0
#define	DEBUG_SPAMLOG	0

#define FUN(_name_) static const char* _func = _name_; Q_UNUSED(_func)

#define	DEBUG_CURSOR	1
#define	DEBUG_UNICODE	0

#if defined(DEBUG_CURSOR) && (DEBUG_CURSOR != 0)
#define DBG_CURSOR(str, ...) qDebug(str, __VA_ARGS__)
#else
#define DBG_CURSOR(str, ...)
#endif

#if defined(DEBUG_UNICODE) && (DEBUG_UNICODE != 0)
#define DBG_UNICODE(str, ...) qDebug(str, __VA_ARGS__)
#else
#define DBG_UNICODE(str, ...)
#endif

vt220::vt220(QWidget* parent)
    : QWidget(parent)
    , m_terminal(VT200)
    , m_font_family(QLatin1String("Fixedsys"))
    , m_backlog_max(10000)
    , m_backlog()
    , m_screen()
    , m_blink_timer(-1)
    , m_screen_time(-1)
    , m_blink_phase(false)
    , m_conceal_off(false)
    , m_zoom(100)
    , m_font_w(font_w)
    , m_font_h(font_h)
    , m_font_d(font_d)
    , m_width(80)
    , m_height(25)
    , m_top(0)
    , m_bottom(25)
    , m_palsize(256)
    , m_pal(m_palsize)
    , m_glyphs()
    , m_def()
    , m_att()
    , m_att_saved()
    , m_cursor()
    , m_cursor_saved()
    , m_cc_mask(0)
    , m_cc_save(0)
    , m_cursor_type(0)
    , m_csi_args()
    , m_tabstop()
    , m_state(ESnormal)
    , m_deccolm(80)
    , m_ques(false)
    , m_decscnm(false)
    , m_togmeta(false)
    , m_deccm(true)
    , m_decim(false)
    , m_decom(false)
    , m_deccr(true)
    , m_decckm(false)
    , m_decawm(true)
    , m_decarm(true)
    , m_repmouse(false)
    , m_dspctrl(false)
    , m_s8c1t(true)
    , m_ansi(3)
    , m_uc(0)
    , m_hc(0)
    , m_bell_pitch(1000)
    , m_bell_duration(100)
    , m_blank_time(10*60)
    , m_vesa_time(10*60)
    , m_charmap_name()
    , m_charmaps(NRCS_COUNT)
    , m_trans()
    , m_utf_mode(true)
    , m_utf_more(0)
    , m_utf_code(UC_INVALID)
    , m_utf_code_min(0)
{
    qDebug("%s: %08x", "CTRL_ACTION", CTRL_ACTION);
    qDebug("%s: %08x", "CTRL_ALWAYS", CTRL_ALWAYS);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    term_reset(m_terminal, m_width, m_height);
    set_font(font_w, font_h, font_d);
    m_blink_timer = startTimer(250);

}

/**
 * @brief Clear the terminal
 * Clear the backlog
 * Erase entire display
 * Cursor position to 1,1
 */
void vt220::clear()
{
    term_reset(VT200, m_width, m_height);
}

QSize vt220::sizeHint() const
{
    QFontMetrics fm = fontMetrics();
    return QSize(m_font_w * m_width,
		 m_font_h * m_height);
}

QString vt220::font_family() const
{
    return m_font_family;
}

void vt220::set_font_family(const QString& family)
{
    m_font_family = family;
    set_font(font_w, font_h, font_d);
    term_set_size(m_width, m_height);
}

int vt220::zoom() const
{
    return m_zoom;
}

void vt220::set_zoom(int percent)
{
    m_zoom = percent;
    set_font(font_w, font_h, font_d);
    term_set_size(m_width, m_height);
}

QSize vt220::term_geometry() const
{
    return QSize(m_font_w * m_width,
		 m_font_h * m_height);
}

bool vt220::event(QEvent* event)
{
    if (event->type() != QEvent::KeyPress) {
	return QWidget::event(event);
    }
    QKeyEvent *ke = static_cast<QKeyEvent *>(event);
    if (ke->key() == Qt::Key_Tab) {
	// special tab handling here
	keyPressEvent(ke);
	return true;
    }
    return QWidget::event(event);
}

void vt220::paintEvent(QPaintEvent* event)
{
    QPainter painter(this);
    const int fw = m_font_w;
    const int fh = m_font_h;
    const int bh = m_backlog.size();
    const QRect rect = event->rect();
    painter.setBackgroundMode(Qt::TransparentMode);
    painter.setClipRect(rect);

    // iterate over rows from rect.top() to rect.bottom()
    for (int sy = (rect.top() / fh) * fh; sy <= rect.bottom(); sy += fh) {
	const int y = sy / fh;	// cell y

	if ((y - bh) >= m_height)
	    break;

	// line attributes
	const vtLine& pl = y < bh ? m_backlog[y] : m_screen[y - bh];

	// skip bottom half of double height lines
	if (pl.bottom())
	    continue;

	const int fwl = pl.decdwl() * fw;
	const int fhl = pl.decdhl() * fh;

	// iterate over columns from rect.left() to rect.right()
	for (int sx = (rect.left() / fwl) * fwl; sx <= rect.right() && sx < m_width * m_font_w; sx += fwl) {
	    const int x = sx / fwl; // cell x
	    if (x >= m_width)
		break;

	    QRect cellrc(sx, sy, fwl, fhl);
	    const vtAttr& pa = pl[x];

	    int bg = pa.bgcolor();
	    int fg = pa.fgcolor() | (pa.faint() ? 0 : 8);
	    int uc = m_uc | (pa.faint() ? 0 : 8);

	    if (pa.inverse() ^ m_decscnm) {
		// inverse mode: swap fore- and background
		std::swap(bg,fg);
		// inverse mode: switch underline color
		uc ^= C_WHT;
	    }

	    if (pa.blink() && m_blink_phase) {
		// blinking mode: currently invisible
		fg = bg;
	    }

	    if (pa.conceal() && !m_conceal_off) {
		// concealed mode: always invisible
		fg = bg;
	    }
	    const QRgb fgcolor = m_pal.value(fg);
	    vtGlyph glyph = m_glyphs.glyph(pa, fgcolor);

	    painter.fillRect(cellrc, QBrush(m_pal.value(bg)));
	    painter.setPen(fgcolor);
	    painter.drawImage(cellrc, glyph.img);

	    // draw an underline?
	    if (pa.underline() && fg != bg) {
		QRect ru = cellrc;
		painter.setPen(m_pal.value(uc));
		ru.setTop(cellrc.bottom() - m_font_d + 1);
		painter.drawLine(ru.topLeft(), ru.topRight());
	    }

	    // draw a double underline?
	    if (pa.underldbl() && fg != bg) {
		QRect ru(cellrc);
		painter.setPen(m_pal.value(uc));
		ru.setTop(cellrc.bottom() - m_font_d + 1);
		ru.setBottom(cellrc.bottom() - 1);
		painter.drawLine(ru.topLeft(), ru.topRight());
		painter.drawLine(ru.bottomLeft(), ru.bottomRight());
	    }

	    // draw a cross through?
	    if (pa.crossed() && fg != bg) {
		QRect rc(cellrc);
		rc.setTop(cellrc.top() + 2);
		rc.setBottom(cellrc.bottom() - 2);
		painter.setPen(m_pal.value(uc));
		painter.drawLine(rc.topLeft(), rc.bottomRight());
		painter.drawLine(rc.bottomLeft(), rc.topRight());
	    }

	    if ((y - bh) == m_cursor.y && x == m_cursor.newx) {
		if (m_cursor.on) {
		    QRgb bgcolor = m_pal.value(bg);
		    QRgb color = qRgb(255 - qRed(bgcolor), 255 - qGreen(bgcolor), 255 - qBlue(bgcolor));
		    // FIXME: cursor type selection
		    vtAttr cur(pa);
		    // cur.set_code(0x2582);   // LOWER ONE QUARTER BLOCK
		    // cur.set_code(0x2595);   // RIGHT ONE EIGHT BLOCK
		    cur.set_code(0x2588);   // FULL BLOCK
		    cur.set_mark(0);
		    vtGlyph block = m_glyphs.glyph(cur, color);
		    painter.drawImage(cellrc, block.img);
		}
	    }
	}
    }
}

void vt220::timerEvent(QTimerEvent* event)
{
    FUN("timerEvent");
    if (m_blink_timer != event->timerId())
	return;
    m_cursor.phase = (m_cursor.phase + 1) % 4;
    set_cursor((m_cursor.phase & 2) ? true : false);
    m_blink_phase = !m_blink_phase;
    update();
}

void vt220::add_backlog(const vtLine& line)
{
    m_backlog.append(line);
    if (m_backlog.count() > m_backlog_max) {
	m_backlog.removeFirst();
    }
    const int bh = m_backlog.size();
    const int sw = m_width * m_font_w;
    const int sh = (bh + m_height) * m_font_h;
    resize(sw, sh);
}

/**
 * @brief Output a cell in the terminal window
 * @param x column
 * @param y row
 * @param pa const reference to a vAttr
 */
void vt220::outch(int x, int y, const vtAttr& pa)
{
    FUN("outch");
    if (x < 0 || x >= m_width)
	return;
    if (y < 0 || y >= m_height)
	return;
    m_screen[y][x] = pa;
    const vtLine& pl = m_screen[y];
    const int bh = m_backlog.size();
    const int fw = m_font_w * pl.decdwl() * pa.width();
    const int fh = m_font_h * pl.decdhl();
    const int x0 = x * m_font_w;
    const int y0 = (bh + y) * m_font_h;
    update(x0, y0, fw, fh);
}

/**
 * @brief Zap a range from x0,y0 to x1,y1 with character @p code
 * @param x0 left column
 * @param y0 top row
 * @param x1 right column
 * @param y1 bottom row
 * @param code Unicode to put in the cells
 */
void vt220::zap(int x0, int y0, int x1, int y1, quint32 code)
{
    const int fw = m_font_w;
    const int fh = m_font_h;
    const int bh = m_backlog.size();
    vtAttr space = m_att;
    space.set_code(code);
    space.set_mark(0);

    QRect upd;
    for (int y = y0; y <= y1; y++) {
	for (int x = x0; x <= x1; x++) {
	    upd = upd.united(QRect(x*fw, (bh+y)*fh, fw, fh));
	    m_screen[y][x] = space;
	}
	x0 = 0;
	x1 = m_width - 1;
    }
    update(upd);
}

/**
 * @brief Set the cursor on or off
 * @param on if true, cursor is visible
 */
void vt220::set_cursor(bool on)
{
    FUN("set_cursor");

    if (m_cursor.y < 0 || m_cursor.y >= m_height) {
	// cursor is off screen rows
	DBG_CURSOR("%s: y (%d) is not in range 0...%d", _func, m_cursor.y, m_height-1);
	return;
    }

    if (m_cursor.newx < 0 || m_cursor.newx >= m_width) {
	// cursor is off screen columns
	DBG_CURSOR("%s: mew (%d) is not in range 0...%d", _func, m_cursor.newx, m_width-1);
	return;
    }

    if (0 == m_deccm) {
	// DEC cursor mode is: off
	DBG_CURSOR("%s: deccm=0", _func);
	on = false;
    }

    // update cursor in terminal
    m_cursor.on = on;
    const vtLine& pl = m_screen[m_cursor.y];
    const int bh = m_backlog.size();
    const int x = m_cursor.newx * m_font_w;
    const int y = (bh + m_cursor.y) * m_font_h;
    const int w = m_font_w * pl.decdwl();
    const int h = m_font_h * pl.decdhl();
    update(x, y, w, h);
}

/**
 * @brief Set a new cursor x coordinate (column) after printing a character
 * @param newx new x coordinate
 */
void vt220::set_newx(int newx)
{
    FUN("set_newx");

    set_cursor(false);
    m_cursor.newx = newx;

    if (m_cursor.y < 0 || m_cursor.y >= m_height) {
	// cursor row is off screen
	DBG_CURSOR("%s: y is not 0 >= %d < %d", _func, m_cursor.y, m_height);
	return;
    }

    if (m_cursor.newx < 0 || m_cursor.newx >= m_width) {
	// cursor column is off screen
	return;
    }

    set_cursor(m_cursor.on);
    const vtLine& pl = m_screen[m_cursor.y];
    const int bh = m_backlog.size();
    const int x = m_cursor.newx * m_font_w;
    const int y = (bh + m_cursor.y) * m_font_h;
    const int w = m_font_w * pl.decdwl();
    const int h = m_font_h * pl.decdhl();
    emit UpdateCursor(QRect(x, y, w, h));
}

/**
 * @brief Scroll the region @ref m_top to @ref m_bottom down
 */
void vt220::vt_scroll_dn()
{
    FUN("vt_scroll_dn");
    vtAttr space = m_att;
    space.set_code(32);
    space.set_mark(0);
    const int fw = m_font_w;
    const int fh = m_font_h;
    const int x0 = 0;
    const int y0 = m_top * fh;
    const int w0 = m_width * fw;
    const int h0 = (m_bottom - m_top) * fh;

    // scroll down the region
    for (int y = m_bottom - 1; y > m_top; y--)
	m_screen[y] = m_screen[y-1];
    vtLine& pl = m_screen[m_top];
    pl.set_decshl();
    pl.set_decswl();
    pl.fill(space, m_width);
    update(x0, y0, w0, h0);
}

/**
 * @brief Scroll the region @ref m_top to @ref m_bottom up
 */
void vt220::vt_scroll_up()
{
    FUN("vt_scroll_up");
    const int fw = m_font_w;
    const int fh = m_font_h;
    const int x0 = 0;
    const int y0 = m_top * fh;
    const int w0 = m_width * fw;
    const int h0 = (m_bottom - m_top) * fh;

    if (0 == m_top && m_height == m_bottom) {
	// scroll up the entire screen
	add_backlog(m_screen[0]);
    }
    for (int y = m_top; y < m_bottom - 1; y++)
	m_screen[y] = m_screen[y+1];
    vtLine& pl = m_screen[m_bottom - 1];
    pl.set_decshl();
    pl.set_decswl();
    vtAttr space(m_att);
    space.set_code(32);
    space.set_mark(0);
    pl.fill(space, m_width);
    update(x0, y0, w0, h0);
}

/**
 * @brief BS - Back Space
 */
void vt220::vt_BS()
{
    FUN("vt_BS");
    bool was_on = m_cursor.on;
    set_cursor(false);
    m_cursor.x = m_cursor.newx;
    if (m_cursor.x > 0) {
	set_newx(m_cursor.x - 1);
    }
    m_cursor.x = m_cursor.newx;
    set_cursor(was_on);
}

/**
 * @brief TAB - Horizontal tabulation
 */
void vt220::vt_TAB()
{
    FUN("vt_TAB");
    qDebug("%s: at newx=%d", _func, m_cursor.newx);
    if (m_cursor.newx >= m_width) {
	// DEC auto wrap mode?
	if (m_decawm) {
	    vt_CR();
	    vt_LF();
	} else {
	    m_cursor.newx = m_width;
	}
    }

    if (m_cursor.newx >= m_width) {
	// cursor is off the last cell on the row
	qDebug("%s: cursor x=%d", _func, m_cursor.newx);
	return;
    }

    m_cursor.x = m_cursor.newx;
    vtAttr space = m_att;
    space.set_code(32);
    space.set_mark(0);
    while (m_cursor.newx < m_width - 1) {
	outch(m_cursor.newx, m_cursor.y, space);
	m_cursor.newx += 1;
	if (m_cursor.newx >= m_tabstop.size() || m_tabstop.testBit(m_cursor.newx))
	    break;
    }
    set_newx(m_cursor.newx);
}

/**
 * @brief RI - reverse index
 */
void vt220::vt_RI()
{
    FUN("vt_RI");
    bool was_on = m_cursor.on;
    set_cursor(false);
    if (m_cursor.y <= m_top) {
	vt_scroll_dn();
	m_cursor.y = m_top;
    } else if (m_cursor.y > 0) {
	m_cursor.y -= 1;
    }
    set_cursor(was_on);
}

/**
 * @brief IND - index
 */
void vt220::vt_IND()
{
    FUN("vt_IND");
    bool was_on = m_cursor.on;
    set_cursor(false);
    if (m_cursor.y + 1 >= m_bottom) {
	vt_scroll_up();
	m_cursor.y = m_bottom - 1;
    } else if (m_cursor.y < m_height - 1) {
	m_cursor.y += 1;
    }
    set_cursor(was_on);
}

/**
 * @brief NEL - next line
 */
void vt220::vt_NEL()
{
    FUN("vt_NEL");
    vt_CR();
    vt_LF();
}

/**
 * @brief HTS - Horizontal Tabulation Set
 */
void vt220::vt_HTS()
{
    FUN("vt_HTS");

    qDebug("%s: set tabstop (%d)", _func, m_cursor.newx);
    if (m_cursor.newx < m_tabstop.size()) {
	m_tabstop.setBit(m_cursor.newx);
    }
}

/**
 * @brief LF - line feed
 */
void vt220::vt_LF()
{
    FUN("vt_LF");
    bool was_on = m_cursor.on;
    set_cursor(false);
    if (m_cursor.y + 1 >= m_bottom) {
	vt_scroll_up();
    } else if (m_cursor.y < m_height - 1) {
	m_cursor.y++;
    }
    set_cursor(was_on);
}

/**
 * @brief VT - cursor up (Vertical Tabulator ?)
 */
void vt220::vt_VT()
{
    FUN("vt_VT");
    bool was_on = m_cursor.on;
    set_cursor(false);
    if (m_cursor.y == m_top) {
	vt_scroll_dn();
    } else if (m_cursor.y > 0) {
	m_cursor.y -= 1;
    }
    set_cursor(was_on);
}

/**
 * @brief FF - Form Feed (new page)
 */
void vt220::vt_FF()
{
    FUN("vt_FF");
    bool was_on = m_cursor.on;
    set_cursor(false);
    // Do it by scrolling the screen out to the backlog
    for (int i = 0; i < m_height; i++) {
	vt_scroll_up();
    }
    m_cursor.y = m_bottom - 1;
    set_cursor(was_on);
}

/**
 * @brief CR - carriage return
 */
void vt220::vt_CR()
{
    FUN("vt_CR");
    bool was_on = m_cursor.on;
    set_cursor(false);
    set_newx(0);
    m_cursor.x = m_cursor.newx;
    set_cursor(was_on);
}

/**
 * @brief LS0 - Invoke the G0 Character Set as GL.
 */
void vt220::vt_LS0()
{
    FUN("vt_LS0");
    m_att.set_gl(0);
    m_att.set_charset(m_att.gl());
    m_dspctrl = false;
    m_trans = m_gmaps[m_att.charset()];
}

/**
 * @brief LS1 - Invoke the G1 Character Set as GL.
 */
void vt220::vt_LS1()
{
    FUN("vt_LS1");
    m_att.set_gl(1);
    m_att.set_charset(m_att.gl());
    m_dspctrl = true;
    m_trans = m_gmaps[m_att.charset()];
}

/**
 * @brief LS2 - Invoke the G2 Character Set as GL.
 */
void vt220::vt_LS2()
{
    FUN("vt_LS2");
    m_att.set_gl(2);
    m_att.set_charset(m_att.gl());
    m_dspctrl = true;
    m_trans = m_gmaps[m_att.charset()];
}

/**
 * @brief LS3 - Invoke the G3 Character Set as GL.
 */
void vt220::vt_LS3()
{
    FUN("vt_LS3");
    m_att.set_gl(3);
    m_att.set_charset(m_att.gl());
    m_dspctrl = true;
    m_trans = m_gmaps[m_att.charset()];
}

/**
 * @brief LS1R - Invoke the G1 Character Set as GR.
 */
void vt220::vt_LS1R()
{
    FUN("vt_LS1R");
    m_att.set_gr(1);
    m_att.set_charset(m_att.gr());
    m_dspctrl = true;
}

/**
 * @brief LS2R - Invoke the G2 Character Set as GR.
 */
void vt220::vt_LS2R()
{
    FUN("vt_LS2R");
    m_att.set_gr(2);
    m_att.set_charset(m_att.gr());
    m_dspctrl = true;
}

/**
 * @brief LS3R - Invoke the G3 Character Set as GR.
 */
void vt220::vt_LS3R()
{
    FUN("vt_LS3R");
    m_att.set_gr(3);
    m_att.set_charset(m_att.gr());
    m_dspctrl = true;
}

/**
 * @brief SS2 - Single Shift to GS 2
 */
void vt220::vt_SS2()
{
    FUN("vt_SS2");
    m_shift = m_att.charset();
    m_att.set_charset(2);
    m_trans = m_gmaps[m_att.charset()];
    m_dspctrl = true;
}

/**
 * @brief SS2 - Single Shift to GS 3
 */
void vt220::vt_SS3()
{
    FUN("vt_SS3");
    m_shift = m_att.charset();
    m_att.set_charset(3);
    m_trans = m_gmaps[m_att.charset()];
    m_dspctrl = true;
}

/**
 * @brief DCS - Device Control String (DCS is 0x90).
 */
void vt220::vt_DCS()
{
    FUN("vt_DCS");

}

/**
 * @brief CUP - cursor position
 * @param x column (zero based)
 * @param y row (zero based)
 */
void vt220::vt_CUP(int x, int y)
{
    FUN("vt_CUP");

    if (x < 0) {
	set_newx(0);
	m_cursor.x = 0;
    } else {
	if (x >= m_width) {
	    set_newx(m_width - 1);
	} else {
	    set_newx(x);
	}
	m_cursor.x = m_cursor.newx;
    }

    // DECOM (origin mode) or all rows?
    int miny = m_decom ? m_top : 0;
    int maxy = m_decom ? m_bottom - 1 : m_height - 1;

    m_cursor.y = qBound(miny, y, maxy);
}

/**
 * @brief CUU cursor up (row)
 * @param n number of rows
 */
void vt220::vt_CUU(int n)
{
    FUN("vt_CUU");
    const int y = m_cursor.y - n;
    m_cursor.x = m_cursor.newx;
    vt_CUP(m_cursor.x, y);
}

/**
 * @brief CUD cursor down
 * @param n number of rows
 */
void vt220::vt_CUD(int n)
{
    FUN("vt_CUD");
    const int y = m_cursor.y + n;
    m_cursor.x = m_cursor.newx;
    vt_CUP(m_cursor.x, y);
}

/**
 * @brief CUF cursor forward
 * @param n number of colums
 */
void vt220::vt_CUF(int n)
{
    FUN("vt_CUF");
    const int x = m_cursor.newx + n;
    vt_CUP(x, m_cursor.y);
}

/**
 * @brief CUB cursor backward
 * @param n number of colums
 */
void vt220::vt_CUB(int n)
{
    FUN("vt_CUB");
    const int x = m_cursor.newx - n;
    vt_CUP(x, m_cursor.y);
}

/**
 * @brief CHA  Cursor Character Absolute  [column] (default = [row,1]) (CHA).
 * @param n column number
 */
void vt220::vt_CHA(int n)
{
    FUN("vt_CHA");
    const int x = n;
    vt_CUP(x, m_cursor.y);
}

/**
 * @brief CNL Cursor Next Line Ps Times (default = 1) (CNL).
 * @param n number of lines
 */
void vt220::vt_CNL(int n)
{
    FUN("vt_CNL");
    while (n-- > 0)
	vt_NEL();
}

/**
 * @brief CPL Cursor Preceding Line Ps Times (default = 1) (CPL).
 * @param n number of lines
 */
void vt220::vt_CPL(int n)
{
    FUN("vt_CPL");
    while (n-- > 0)
	vt_RI();
}

/**
 * @brief CHT Cursor Forward Tabulation Ps tab stops (default = 1) (CHT).
 * @param n number of tab stops
 */
void vt220::vt_CHT(int n)
{
    FUN("vt_CHT");
    while (n-- > 0)
	vt_TAB();
}

/**
 * @brief CUA cursor address
 * @param x cursor column (zero based)
 * @param y cursor row (zero based)
 */
void vt220::vt_CUA(int x, int y)
{
    FUN("vt_CUA");
    vt_CUP(x, m_decom ? m_top + y : y);
}

/**
 * @brief DECSET DEC Set Mode (Handle <CSI>...h arguments)
 */
void vt220::vt_DECSET()
{
    FUN("vt_DECSET");

    // Always at least 1 argument
    if (m_csi_args.count() < 1)
	m_csi_args.resize(1);

    for (int i = 0; i < m_csi_args.count(); i++) {
	if (m_ques) {
	    switch (m_csi_args[i]) {
	    case 1:	// cursor keys send ^[0x ... ^[[x
		m_decckm = true;
		break;
	    case 2:	// Designate USASCII for character sets G0-G3
		m_gmaps[0] = m_gmaps[1] = m_gmaps[2] = m_gmaps[3] = m_charmaps[NRCS_USASCII];
		break;
	    case 3:	// 80/132 mode
		m_deccolm = 132;
		term_set_columns();
		break;
	    case 5:	// inverted screen on/of
		m_decscnm = true;
		break;
	    case 6:	// origin relative/absolute
		m_decom = true;
		vt_CUA(0, 0);
		break;
	    case 7:	// auto wrap on/off
		m_decawm = true;
		break;
	    case 8:	// keyboard autorepeat on/off
		m_decarm = true;
		break;
	    case 9:	// report mouse on/off
		m_repmouse = true;
		break;
	    case 25:	// cursor on off
		m_deccm = true;
		break;
	    default:
		qCritical("%s: invalid #%d Ps: %d", _func,
			    i, m_csi_args[i]);
	    }
	} else {
	    switch (m_csi_args[i]) {
	    case 3:		// monitor display controls
		m_dspctrl = true;
		break;
	    case 4:		// insert mode on/off
		m_decim = true;
		break;
	    case 20:		// keyboard enter = CRLF/LF
		m_deccr = true;
		break;
	    default:
		qCritical("%s: invalid #%d Ps: %d", _func,
			    i, m_csi_args[i]);
	    }
	}
    }
}

/**
 * @brief RM Reset Mode (Handle <CSI>...l arguments)
 * @param on true for 'h', false for 'l'
 */
void vt220::vt_RM()
{
    FUN("vt_RM");

    // Always at least 1 argument
    if (m_csi_args.count() < 1)
	m_csi_args.resize(1);

    for (int i = 0; i < m_csi_args.count(); i++) {
	if (m_ques) {
	    switch (m_csi_args[i]) {
	    case 1:	// cursor keys send ^[0x ... ^[[x
		m_decckm = false;
		break;
	    case 2:	// Designate USASCII for character sets G0-G3
		break;
	    case 3:	// 80/132 mode
		m_deccolm = 80;
		term_set_columns();
		break;
	    case 5:	// inverted screen on/of
		m_decscnm = false;
		break;
	    case 6:	// origin relative/absolute
		m_decom = false;
		vt_CUA(0, 0);
		break;
	    case 7:	// auto wrap on/off
		m_decawm = false;
		break;
	    case 8:	// keyboard autorepeat on/off
		m_decarm = false;
		break;
	    case 9:	// report mouse on/off
		m_repmouse = false;
		break;
	    case 25:	// cursor on off
		m_deccm = false;
		break;
	    default:
		qCritical("%s: invalid #%d Ps: %d", _func,
			    i, m_csi_args[i]);
	    }
	} else {
	    switch (m_csi_args[i]) {
	    case 3:		// monitor display controls
		m_dspctrl = false;
		break;
	    case 4:		// insert mode on/off
		m_decim = false;
		break;
	    case 20:		// keyboard enter = CRLF/LF
		m_deccr = false;
		break;
	    default:
		qCritical("%s: invalid #%d Ps: %d", _func,
			    i, m_csi_args[i]);
	    }
	}
    }
}

/**
 * @brief ED - Erase Display
 * @param n argument
 */
void vt220::vt_ED(int n)
{
    FUN("vt_ED");

    m_cursor.x = m_cursor.newx;
    switch (n) {
    case 0:		// erase cursor to end of display
	zap(m_cursor.x, m_cursor.y, m_width-1, m_height-1, 0x0020);
	break;

    case 1:		// erase from start of display to cursor
	zap(0, 0, m_cursor.x, m_cursor.y, 0x0020);
	break;

    case 2:		// erase entire display
	zap(0, 0, m_width-1, m_height-1, 0x0020);
	break;

    default:
	qCritical("%s: invalid range %d", _func, n);
    }
}

/**
 * @brief EL - Erase Line(s)
 * @param n argument
 */
void vt220::vt_EL(int n)
{
    FUN("vt_EL");

    m_cursor.x = m_cursor.newx;
    switch (n) {
    case 0:		// erase from cursor to end of line
	zap(m_cursor.x, m_cursor.y, m_width-1, m_cursor.y, 0x0020);
	break;

    case 1:		// erase from start of line to cursor
	zap(0, m_cursor.y, m_cursor.x, m_cursor.y, 0x0020);
	break;

    case 2:		// erase the entire line
	zap(0, m_cursor.y, m_width-1, m_cursor.y, 0x0020);
	break;

    default:
	qCritical("%s: invalid range %d", _func, n);
    }
}


/**
 * @brief IL - Insert Line(s) at cursor row
 * @param n number of lines to insert
 */
void vt220::vt_IL(int n)
{
    FUN("vt_CSI_L");
    const int top = m_top;

    m_cursor.x = m_cursor.newx;
    m_top = m_cursor.y;

    while (n-- > 0)
	vt_scroll_dn();

    m_top = top;
}

/**
 * @brief DL - Delete Line(s)
 * @param n number of lines to delete
 */
void vt220::vt_DL(int n)
{
    FUN("vt_DL");
    const int top = m_top;
    m_cursor.x = m_cursor.newx;
    m_top = m_cursor.y;

    while (n-- > 0)
	vt_scroll_up();

    m_top = top;
}

/**
 * @brief ICH - Insert Character(s) at the cursor position
 * @param n number of blanks to insert
 */
void vt220::vt_ICH(int n)
{
    FUN("vt_ICH");

    m_cursor.x = m_cursor.newx;
    // insert n blanks at the cursor position
    while (n-- > 0) {
	for (int x = m_width - 1; x > m_cursor.x; x--)
	    outch(x, m_cursor.y, m_screen[m_cursor.y][x-1]);
	vtAttr space = m_att;
	space.set_code(32);
	space.set_mark(0);
	outch(m_cursor.x, m_cursor.y, space);
    }
}

/**
 * @brief DCH - Delete Character(s) at the cursor position
 * @param n number of characters to delete
 */
void vt220::vt_DCH(int n)
{
    FUN("vt_DCH");

    m_cursor.x = m_cursor.newx;
    // delete n characters at the cursor position
    while (n-- > 0) {
	for (int x = m_cursor.x; x < m_width - 1; x++)
	    outch(x, m_cursor.y, m_screen[m_cursor.y][x+1]);
	vtAttr space = m_att;
	space.set_code(32);
	space.set_mark(0);
	outch(m_width - 1, m_cursor.y, space);
    }
}

/**
 * @brief CSI X (ECH) - erase n characters at the cursor position
 * @param n
 */
void vt220::vt_ECH(int n)
{
    FUN("vt_ECH");

    m_cursor.x = m_cursor.newx;

    vtAttr space = m_att;
    space.set_code(32);
    space.set_mark(0);
    outch(m_width - 1, m_cursor.y, space);

    // earse n characters at the cursor position
    for (int x = m_cursor.x; x < m_cursor.x + n && x < m_width; x++)
	outch(x, m_cursor.y, space);
}

/**
 * @brief CSI V (SPA) - start of protected area
 */
void vt220::vt_SPA()
{
    FUN("vt_SPA");
}

/**
 * @brief CSI W (EPA) - end of protected area
 */
void vt220::vt_EPA()
{
    FUN("vt_EPA");
}

/**
 * @brief CSI X (SOS) - start of string
 */
void vt220::vt_SOS()
{
    FUN("vt_SOS");
    m_string.clear();
    m_state = ESstring;
}

/**
 * @brief CSI \ (ST) - start of string
 */
void vt220::vt_ST()
{
    FUN("vt_ST");
    qDebug("%s: string=\"%s\"", _func, qPrintable(m_string));
    m_state = ESnormal;
}

void vt220::vt_PM()
{
    FUN("vt_PM");
    m_state = ESnormal;
}

void vt220::vt_APC()
{
    FUN("vt_APC");
    m_state = ESnormal;
}

/**
 * @brief Select a character map based on the last character of an ESC sequence
 * @param ch last character
 * @return CharacterMap to use
 */
vt220::CharacterMap vt220::select_map_vt200(uchar ch)
{
    CharacterMap map = MAP_LATIN1;
    switch (ch) {
    case '0':	// graphics map
	map = MAP_DECGR;
	break;
    case 'A':	// Latin-1 map British
	map = NRCS_BRITISH;
	break;
    case 'B':	// Latin-1 map EN-US
	map = MAP_LATIN1;
	break;
    case 'C':	// Latin-1 map Finnish (VT200)
    case '5':	// alternate value
	map = NRCS_FINNISH;
	break;
    case 'E':	// Latin-1 map Norwegian/Danish (VT200)
    case '6':	// alternate value
    case '`':	// alternate value
	map = NRCS_NORWEGIAN_DANISH;
	break;
    case 'H':	// Latin-1 map Swedish (VT200)
    case '7':	// alternate value
	map = NRCS_SWEDISH;
	break;
    case 'K':	// Latin-1 map German (VT200)
	map = NRCS_GERMAN;
	break;
    case 'Q':	// Latin-1 map French Candian (VT200)
    case '9':	// alternate value
	map = NRCS_FRENCH_CANADIAN;
	break;
    case 'R':	// Latin-1 map French (VT200)
    case 'f':	// alternate value
	map = NRCS_FRENCH;
	break;
    case 'U':	// IBMPC map
	map = MAP_IBMPC;
	break;
    case 'Y':	// Latin-1 map Italian (VT200)
	map = NRCS_ITALIAN;
	break;
    case 'Z':	// Latin-1 map Spanish (VT200)
	map = NRCS_SPANISH;
	break;
    case '4':	// Latin-1 map Dutch (VT200)
	map = NRCS_DUTCH;
	break;
    case '%':	// VT500 selection
	map = MAP_VT500;
	break;
    case '=':	// Latin-1 map Swiss (VT420)
	map = NRCS_SWISS;
	break;
    }
    return map;
}

vt220::CharacterMap vt220::select_map_vt500(uchar ch)
{
    CharacterMap map = MAP_INVALID;
    switch (ch) {
    case '>':	// "> Latin-1 map Greek (VT510)
	map = NRCS_GREEK;
	break;
    case '2':	// %2 Latin-1 map Turkish (VT500)
	map = NRCS_TURKISH;
	break;
    case '6':	// %2 Latin-1 map Portuguese (VT320)
	map = NRCS_PORTUGESE;
	break;
    case '=':	// %2 Latin-1 map Hebrew (VT520)
	map = NRCS_HEBREW;
	break;
    }
    return map;
}

/**
 * @brief SGR - Set character attributes
 */
void vt220::vt_SGR()
{
    FUN("vt_SGR");

    // Always at least 1 argument
    if (m_csi_args.count() < 1)
	m_csi_args.resize(1);

    for (int i = 0; i < m_csi_args.count(); i++) {
	switch (m_csi_args[i]) {
	case 0:	    // reset to defaults
	    m_att.set_all(m_def);
	    break;
	case 1:	    // highlight on
	    m_att.set_bold(true);
	    break;
	case 2:	    // faint on
	    m_att.set_faint(true);
	    break;
	case 3:	    // italicized on
	    m_att.set_italic(true);
	    break;
	case 4:	    // underline on
	    m_att.set_underline(true);
	    break;
	case 5:	    // blinking on
	    m_att.set_blink(true);
	    break;
	case 7:	    // inverse on
	    m_att.set_inverse(true);
	    break;
	case 8:	    // concealed on
	    m_att.set_conceal(true);
	    break;
	case 9:	    // crossed out on
	    m_att.set_crossed(true);
	    break;
	case 10:    // select primary font, no control chars, reset togmeta
	    m_trans = m_gmaps[m_att.charset() ? m_att.gr() : m_att.gl()];
	    m_dspctrl = false;
	    m_togmeta = false;
	    break;
	case 11:    // select alternate font, display control chars
	    m_trans = m_gmaps[MAP_IBMPC];
	    m_dspctrl = true;
	    m_togmeta = false;
	    break;
	case 12:    // select alternate font, display high bit chars
	    m_trans = m_gmaps[MAP_IBMPC];
	    m_dspctrl = true;
	    m_togmeta = true;
	    break;
	case 21:    // doubly underlined
	    m_att.set_underldbl(true);
	    break;
	case 22:    // normal (neither bold nor faint)
	    m_att.set_bold(false);
	    m_att.set_faint(false);
	    break;
	case 23:    // italicized off
	    m_att.set_italic(false);
	    break;
	case 24:    // underline off
	    m_att.set_underline(false);
	    m_att.set_underldbl(false);
	    break;
	case 25:    // steady (blinking off)
	    m_att.set_blink(false);
	    break;
	case 27:    // positive (inverse off)
	    m_att.set_inverse(false);
	    break;
	case 28:    // visible (concealed off)
	    m_att.set_conceal(false);
	    break;
	case 29:    // not crossed out (crossed out off)
	    m_att.set_crossed(false);
	    break;
	case 30:    // set foreground color to black
	    m_att.set_fgcolor(C_BLK);
	    break;
	case 31:    // set foreground color to red
	    m_att.set_fgcolor(C_RED);
	    break;
	case 32:    // set foreground color to green
	    m_att.set_fgcolor(C_GRN);
	    break;
	case 33:    // set foreground color to yellow
	    m_att.set_fgcolor(C_YEL);
	    break;
	case 34:    // set foreground color to blue
	    m_att.set_fgcolor(C_BLU);
	    break;
	case 35:    // set foreground color to magenta
	    m_att.set_fgcolor(C_MAG);
	    break;
	case 36:    // set foreground color to cyan
	    m_att.set_fgcolor(C_CYN);
	    break;
	case 37:    // set foreground color to white
	    m_att.set_fgcolor(C_WHT);
	    break;
	case 38:    // set default color, underline on
	    m_att.set_fgcolor(m_def.fgcolor());
	    m_att.set_underline(true);
	    break;
	case 39:    // set default color, underline off
	    m_att.set_fgcolor(m_def.fgcolor());
	    m_att.set_underline(false);
	    break;
	case 40:    // set background color to black
	    m_att.set_bgcolor(C_BLK);
	    break;
	case 41:    // set background color to red
	    m_att.set_bgcolor(C_RED);
	    break;
	case 42:    // set background color to green
	    m_att.set_bgcolor(C_GRN);
	    break;
	case 43:    // set background color to yellow
	    m_att.set_bgcolor(C_YEL);
	    break;
	case 44:    // set background color to blue
	    m_att.set_bgcolor(C_BLU);
	    break;
	case 45:    // set background color to magenta
	    m_att.set_bgcolor(C_MAG);
	    break;
	case 46:    // set background color to cyan
	    m_att.set_bgcolor(C_CYN);
	    break;
	case 47:    // set background color to white
	    m_att.set_bgcolor(C_WHT);
	    break;
	case 49:    // set background color to default
	    m_att.set_bgcolor(m_def.bgcolor());
	    break;
	default:
	    qCritical("%s: invalid <CSI>%dm", _func, m_csi_args[i]);
	}
    }
}

/**
 * @brief OSC - Operating System Command
 * @return VT_SUCCESS on success
 */
void vt220::vt_OSC()
{
    FUN("vt_OSC");

    // Always at least 1 argument
    if (m_csi_args.count() < 1)
	m_csi_args.resize(1);

    switch (m_csi_args[0]) {
    case 1:	// set underline color
	m_uc = m_csi_args[1] & 7;
	break;
    case 2:	// set half-bright color
	m_hc = m_csi_args[1] & 7;
	break;
    case 8:	// store attributes as default
	m_def = m_att;
	break;
    case 9:	// set blanking interval
	m_blank_time = (m_csi_args.count() < 2 ? 60 : m_csi_args[1] < 60 ? m_csi_args[1] : 60) * 60;
	break;
    case 10:	// set bell pitch
	m_bell_pitch = m_csi_args.count() < 2 ? 750 : m_csi_args[1];
	break;
    case 11:	// set bell duration
	m_bell_duration = m_csi_args.count() < 2 ? 125 : m_csi_args[1] < 2000 ? m_csi_args[1] : 2000;
	break;
    case 12:	// bring the specified console to the front
#if	0
	if (m_par[1] >= 1 && m_par[1] <= 8)
	    vt_console(m_par[1]);
#endif
	break;
    case 13:	// unblank the screen
	break;
    case 14:	// set VESA powerdown interval
	m_csi_args.resize(2);
	m_vesa_time = (m_csi_args[1] < 60 ? m_csi_args[1] : 60) * 60;
	break;
    default:
	qCritical("%s: invalid <CSI>%dm", _func, m_csi_args[0]);
    }
}

void vt220::vt_palette_reset()
{
    FUN("vt_palette_reset");

    switch (m_palsize) {
    case 88:
	m_pal = {
	    qRgb(0x00,0x00,0x00), qRgb(0xaa,0x00,0x00), qRgb(0x00,0xaa,0x00), qRgb(0xaa,0x55,0x00),
	    qRgb(0x00,0x00,0xaa), qRgb(0xaa,0x00,0xaa), qRgb(0x00,0xaa,0xaa), qRgb(0xaa,0xaa,0xaa),
	    qRgb(0x55,0x55,0x55), qRgb(0xff,0x55,0x55), qRgb(0x55,0xff,0x55), qRgb(0xff,0xff,0x55),
	    qRgb(0x55,0x55,0xff), qRgb(0xff,0x55,0xff), qRgb(0x55,0xff,0xff), qRgb(0xff,0xff,0xff),
	    qRgb(0x00,0x00,0x00), qRgb(0x00,0x00,0x8b), qRgb(0x00,0x00,0xcd), qRgb(0x00,0x00,0xff),
	    qRgb(0x00,0x8b,0x00), qRgb(0x00,0x8b,0x8b), qRgb(0x00,0x8b,0xcd), qRgb(0x00,0x8b,0xff),
	    qRgb(0x00,0xcd,0x00), qRgb(0x00,0xcd,0x8b), qRgb(0x00,0xcd,0xcd), qRgb(0x00,0xcd,0xff),
	    qRgb(0x00,0xff,0x00), qRgb(0x00,0xff,0x8b), qRgb(0x00,0xff,0xcd), qRgb(0x00,0xff,0xff),
	    qRgb(0x8b,0x00,0x00), qRgb(0x8b,0x00,0x8b), qRgb(0x8b,0x00,0xcd), qRgb(0x8b,0x00,0xff),
	    qRgb(0x8b,0x8b,0x00), qRgb(0x8b,0x8b,0x8b), qRgb(0x8b,0x8b,0xcd), qRgb(0x8b,0x8b,0xff),
	    qRgb(0x8b,0xcd,0x00), qRgb(0x8b,0xcd,0x8b), qRgb(0x8b,0xcd,0xcd), qRgb(0x8b,0xcd,0xff),
	    qRgb(0x8b,0xff,0x00), qRgb(0x8b,0xff,0x8b), qRgb(0x8b,0xff,0xcd), qRgb(0x8b,0xff,0xff),
	    qRgb(0xcd,0x00,0x00), qRgb(0xcd,0x00,0x8b), qRgb(0xcd,0x00,0xcd), qRgb(0xcd,0x00,0xff),
	    qRgb(0xcd,0x8b,0x00), qRgb(0xcd,0x8b,0x8b), qRgb(0xcd,0x8b,0xcd), qRgb(0xcd,0x8b,0xff),
	    qRgb(0xcd,0xcd,0x00), qRgb(0xcd,0xcd,0x8b), qRgb(0xcd,0xcd,0xcd), qRgb(0xcd,0xcd,0xff),
	    qRgb(0xcd,0xff,0x00), qRgb(0xcd,0xff,0x8b), qRgb(0xcd,0xff,0xcd), qRgb(0xcd,0xff,0xff),
	    qRgb(0xff,0x00,0x00), qRgb(0xff,0x00,0x8b), qRgb(0xff,0x00,0xcd), qRgb(0xff,0x00,0xff),
	    qRgb(0xff,0x8b,0x00), qRgb(0xff,0x8b,0x8b), qRgb(0xff,0x8b,0xcd), qRgb(0xff,0x8b,0xff),
	    qRgb(0xff,0xcd,0x00), qRgb(0xff,0xcd,0x8b), qRgb(0xff,0xcd,0xcd), qRgb(0xff,0xcd,0xff),
	    qRgb(0xff,0xff,0x00), qRgb(0xff,0xff,0x8b), qRgb(0xff,0xff,0xcd), qRgb(0xff,0xff,0xff),
	    qRgb(0x2e,0x2e,0x2e), qRgb(0x5c,0x5c,0x5c), qRgb(0x73,0x73,0x73), qRgb(0x8b,0x8b,0x8b),
	    qRgb(0xa2,0xa2,0xa2), qRgb(0xb9,0xb9,0xb9), qRgb(0xd0,0xd0,0xd0), qRgb(0xe7,0xe7,0xe7)
	};
	break;

    case 256:
	m_pal = {
	    qRgb(0x00,0x00,0x00), qRgb(0xaa,0x00,0x00), qRgb(0x00,0xaa,0x00), qRgb(0xaa,0x55,0x00),
	    qRgb(0x00,0x00,0xaa), qRgb(0xaa,0x00,0xaa), qRgb(0x00,0xaa,0xaa), qRgb(0xaa,0xaa,0xaa),
	    qRgb(0x55,0x55,0x55), qRgb(0xff,0x55,0x55), qRgb(0x55,0xff,0x55), qRgb(0xff,0xff,0x55),
	    qRgb(0x55,0x55,0xff), qRgb(0xff,0x55,0xff), qRgb(0x55,0xff,0xff), qRgb(0xff,0xff,0xff),
	    qRgb(0x00,0x00,0x00), qRgb(0x00,0x00,0x5f), qRgb(0x00,0x00,0x87), qRgb(0x00,0x00,0xaf),
	    qRgb(0x00,0x00,0xd7), qRgb(0x00,0x00,0xff), qRgb(0x00,0x5f,0x00), qRgb(0x00,0x5f,0x5f),
	    qRgb(0x00,0x5f,0x87), qRgb(0x00,0x5f,0xaf), qRgb(0x00,0x5f,0xd7), qRgb(0x00,0x5f,0xff),
	    qRgb(0x00,0x87,0x00), qRgb(0x00,0x87,0x5f), qRgb(0x00,0x87,0x87), qRgb(0x00,0x87,0xaf),
	    qRgb(0x00,0x87,0xd7), qRgb(0x00,0x87,0xff), qRgb(0x00,0xaf,0x00), qRgb(0x00,0xaf,0x5f),
	    qRgb(0x00,0xaf,0x87), qRgb(0x00,0xaf,0xaf), qRgb(0x00,0xaf,0xd7), qRgb(0x00,0xaf,0xff),
	    qRgb(0x00,0xd7,0x00), qRgb(0x00,0xd7,0x5f), qRgb(0x00,0xd7,0x87), qRgb(0x00,0xd7,0xaf),
	    qRgb(0x00,0xd7,0xd7), qRgb(0x00,0xd7,0xff), qRgb(0x00,0xff,0x00), qRgb(0x00,0xff,0x5f),
	    qRgb(0x00,0xff,0x87), qRgb(0x00,0xff,0xaf), qRgb(0x00,0xff,0xd7), qRgb(0x00,0xff,0xff),
	    qRgb(0x5f,0x00,0x00), qRgb(0x5f,0x00,0x5f), qRgb(0x5f,0x00,0x87), qRgb(0x5f,0x00,0xaf),
	    qRgb(0x5f,0x00,0xd7), qRgb(0x5f,0x00,0xff), qRgb(0x5f,0x5f,0x00), qRgb(0x5f,0x5f,0x5f),
	    qRgb(0x5f,0x5f,0x87), qRgb(0x5f,0x5f,0xaf), qRgb(0x5f,0x5f,0xd7), qRgb(0x5f,0x5f,0xff),
	    qRgb(0x5f,0x87,0x00), qRgb(0x5f,0x87,0x5f), qRgb(0x5f,0x87,0x87), qRgb(0x5f,0x87,0xaf),
	    qRgb(0x5f,0x87,0xd7), qRgb(0x5f,0x87,0xff), qRgb(0x5f,0xaf,0x00), qRgb(0x5f,0xaf,0x5f),
	    qRgb(0x5f,0xaf,0x87), qRgb(0x5f,0xaf,0xaf), qRgb(0x5f,0xaf,0xd7), qRgb(0x5f,0xaf,0xff),
	    qRgb(0x5f,0xd7,0x00), qRgb(0x5f,0xd7,0x5f), qRgb(0x5f,0xd7,0x87), qRgb(0x5f,0xd7,0xaf),
	    qRgb(0x5f,0xd7,0xd7), qRgb(0x5f,0xd7,0xff), qRgb(0x5f,0xff,0x00), qRgb(0x5f,0xff,0x5f),
	    qRgb(0x5f,0xff,0x87), qRgb(0x5f,0xff,0xaf), qRgb(0x5f,0xff,0xd7), qRgb(0x5f,0xff,0xff),
	    qRgb(0x87,0x00,0x00), qRgb(0x87,0x00,0x5f), qRgb(0x87,0x00,0x87), qRgb(0x87,0x00,0xaf),
	    qRgb(0x87,0x00,0xd7), qRgb(0x87,0x00,0xff), qRgb(0x87,0x5f,0x00), qRgb(0x87,0x5f,0x5f),
	    qRgb(0x87,0x5f,0x87), qRgb(0x87,0x5f,0xaf), qRgb(0x87,0x5f,0xd7), qRgb(0x87,0x5f,0xff),
	    qRgb(0x87,0x87,0x00), qRgb(0x87,0x87,0x5f), qRgb(0x87,0x87,0x87), qRgb(0x87,0x87,0xaf),
	    qRgb(0x87,0x87,0xd7), qRgb(0x87,0x87,0xff), qRgb(0x87,0xaf,0x00), qRgb(0x87,0xaf,0x5f),
	    qRgb(0x87,0xaf,0x87), qRgb(0x87,0xaf,0xaf), qRgb(0x87,0xaf,0xd7), qRgb(0x87,0xaf,0xff),
	    qRgb(0x87,0xd7,0x00), qRgb(0x87,0xd7,0x5f), qRgb(0x87,0xd7,0x87), qRgb(0x87,0xd7,0xaf),
	    qRgb(0x87,0xd7,0xd7), qRgb(0x87,0xd7,0xff), qRgb(0x87,0xff,0x00), qRgb(0x87,0xff,0x5f),
	    qRgb(0x87,0xff,0x87), qRgb(0x87,0xff,0xaf), qRgb(0x87,0xff,0xd7), qRgb(0x87,0xff,0xff),
	    qRgb(0xaf,0x00,0x00), qRgb(0xaf,0x00,0x5f), qRgb(0xaf,0x00,0x87), qRgb(0xaf,0x00,0xaf),
	    qRgb(0xaf,0x00,0xd7), qRgb(0xaf,0x00,0xff), qRgb(0xaf,0x5f,0x00), qRgb(0xaf,0x5f,0x5f),
	    qRgb(0xaf,0x5f,0x87), qRgb(0xaf,0x5f,0xaf), qRgb(0xaf,0x5f,0xd7), qRgb(0xaf,0x5f,0xff),
	    qRgb(0xaf,0x87,0x00), qRgb(0xaf,0x87,0x5f), qRgb(0xaf,0x87,0x87), qRgb(0xaf,0x87,0xaf),
	    qRgb(0xaf,0x87,0xd7), qRgb(0xaf,0x87,0xff), qRgb(0xaf,0xaf,0x00), qRgb(0xaf,0xaf,0x5f),
	    qRgb(0xaf,0xaf,0x87), qRgb(0xaf,0xaf,0xaf), qRgb(0xaf,0xaf,0xd7), qRgb(0xaf,0xaf,0xff),
	    qRgb(0xaf,0xd7,0x00), qRgb(0xaf,0xd7,0x5f), qRgb(0xaf,0xd7,0x87), qRgb(0xaf,0xd7,0xaf),
	    qRgb(0xaf,0xd7,0xd7), qRgb(0xaf,0xd7,0xff), qRgb(0xaf,0xff,0x00), qRgb(0xaf,0xff,0x5f),
	    qRgb(0xaf,0xff,0x87), qRgb(0xaf,0xff,0xaf), qRgb(0xaf,0xff,0xd7), qRgb(0xaf,0xff,0xff),
	    qRgb(0xd7,0x00,0x00), qRgb(0xd7,0x00,0x5f), qRgb(0xd7,0x00,0x87), qRgb(0xd7,0x00,0xaf),
	    qRgb(0xd7,0x00,0xd7), qRgb(0xd7,0x00,0xff), qRgb(0xd7,0x5f,0x00), qRgb(0xd7,0x5f,0x5f),
	    qRgb(0xd7,0x5f,0x87), qRgb(0xd7,0x5f,0xaf), qRgb(0xd7,0x5f,0xd7), qRgb(0xd7,0x5f,0xff),
	    qRgb(0xd7,0x87,0x00), qRgb(0xd7,0x87,0x5f), qRgb(0xd7,0x87,0x87), qRgb(0xd7,0x87,0xaf),
	    qRgb(0xd7,0x87,0xd7), qRgb(0xd7,0x87,0xff), qRgb(0xd7,0xaf,0x00), qRgb(0xd7,0xaf,0x5f),
	    qRgb(0xd7,0xaf,0x87), qRgb(0xd7,0xaf,0xaf), qRgb(0xd7,0xaf,0xd7), qRgb(0xd7,0xaf,0xff),
	    qRgb(0xd7,0xd7,0x00), qRgb(0xd7,0xd7,0x5f), qRgb(0xd7,0xd7,0x87), qRgb(0xd7,0xd7,0xaf),
	    qRgb(0xd7,0xd7,0xd7), qRgb(0xd7,0xd7,0xff), qRgb(0xd7,0xff,0x00), qRgb(0xd7,0xff,0x5f),
	    qRgb(0xd7,0xff,0x87), qRgb(0xd7,0xff,0xaf), qRgb(0xd7,0xff,0xd7), qRgb(0xd7,0xff,0xff),
	    qRgb(0xff,0x00,0x00), qRgb(0xff,0x00,0x5f), qRgb(0xff,0x00,0x87), qRgb(0xff,0x00,0xaf),
	    qRgb(0xff,0x00,0xd7), qRgb(0xff,0x00,0xff), qRgb(0xff,0x5f,0x00), qRgb(0xff,0x5f,0x5f),
	    qRgb(0xff,0x5f,0x87), qRgb(0xff,0x5f,0xaf), qRgb(0xff,0x5f,0xd7), qRgb(0xff,0x5f,0xff),
	    qRgb(0xff,0x87,0x00), qRgb(0xff,0x87,0x5f), qRgb(0xff,0x87,0x87), qRgb(0xff,0x87,0xaf),
	    qRgb(0xff,0x87,0xd7), qRgb(0xff,0x87,0xff), qRgb(0xff,0xaf,0x00), qRgb(0xff,0xaf,0x5f),
	    qRgb(0xff,0xaf,0x87), qRgb(0xff,0xaf,0xaf), qRgb(0xff,0xaf,0xd7), qRgb(0xff,0xaf,0xff),
	    qRgb(0xff,0xd7,0x00), qRgb(0xff,0xd7,0x5f), qRgb(0xff,0xd7,0x87), qRgb(0xff,0xd7,0xaf),
	    qRgb(0xff,0xd7,0xd7), qRgb(0xff,0xd7,0xff), qRgb(0xff,0xff,0x00), qRgb(0xff,0xff,0x5f),
	    qRgb(0xff,0xff,0x87), qRgb(0xff,0xff,0xaf), qRgb(0xff,0xff,0xd7), qRgb(0xff,0xff,0xff),
	    qRgb(0x08,0x08,0x08), qRgb(0x12,0x12,0x12), qRgb(0x1c,0x1c,0x1c), qRgb(0x26,0x26,0x26),
	    qRgb(0x30,0x30,0x30), qRgb(0x3a,0x3a,0x3a), qRgb(0x44,0x44,0x44), qRgb(0x4e,0x4e,0x4e),
	    qRgb(0x58,0x58,0x58), qRgb(0x62,0x62,0x62), qRgb(0x6c,0x6c,0x6c), qRgb(0x76,0x76,0x76),
	    qRgb(0x80,0x80,0x80), qRgb(0x8a,0x8a,0x8a), qRgb(0x94,0x94,0x94), qRgb(0x9e,0x9e,0x9e),
	    qRgb(0xa8,0xa8,0xa8), qRgb(0xb2,0xb2,0xb2), qRgb(0xbc,0xbc,0xbc), qRgb(0xc6,0xc6,0xc6),
	    qRgb(0xd0,0xd0,0xd0), qRgb(0xda,0xda,0xda), qRgb(0xe4,0xe4,0xe4), qRgb(0xee,0xee,0xee)
	};
	break;

    case 16:
    default:
	m_pal = {
	    qRgb(0x00,0x00,0x00), qRgb(0xaa,0x00,0x00), qRgb(0x00,0xaa,0x00), qRgb(0xaa,0x55,0x00),
	    qRgb(0x00,0x00,0xaa), qRgb(0xaa,0x00,0xaa), qRgb(0x00,0xaa,0xaa), qRgb(0xaa,0xaa,0xaa),
	    qRgb(0x55,0x55,0x55), qRgb(0xff,0x55,0x55), qRgb(0x55,0xff,0x55), qRgb(0xff,0xff,0x55),
	    qRgb(0x55,0x55,0xff), qRgb(0xff,0x55,0xff), qRgb(0x55,0xff,0xff), qRgb(0xff,0xff,0xff),
	};
	break;
    }
}

void vt220::vt_tabstop_reset()
{
    FUN("vt_tabstop_reset");
    m_tabstop.fill(false, 256);
    for (int i = 8; i < 256; i += 8)
	m_tabstop[i] = true;
}

void vt220::set_font(int width, int height, int descend)
{
    QFont font;
    m_font_w = width * m_zoom / 100;
    m_font_h = height * m_zoom / 100;
    m_font_d = descend * m_zoom / 100;
    QImage img(m_font_w, m_font_h, QImage::Format_ARGB32);
    if (m_font_family.isEmpty()) {
	font = QFont(QFontDatabase::systemFont(QFontDatabase::FixedFont), &img);
    } else {
	font = QFont(m_font_family, &img);
    }
    font.setFixedPitch(true);
    font.setStretch(130);
    font.setWeight(QFont::Light);
    font.setPixelSize(m_font_h - m_font_d);
    setFont(font);
#if DEBUG_FONTINFO
    qDebug("%s: using family '%s'", __func__, qPrintable(font.family()));
    qDebug("%s:     fixed pitch : %s", __func__, font.fixedPitch() ? "true" : "false");
    qDebug("%s:     kerning     : %s", __func__, font.kerning() ? "true" : "false");
    qDebug("%s:     weight      : %d", __func__, font.weight());
    qDebug("%s:     pixel size  : %d", __func__, font.pixelSize());
    qDebug("%s:     stretch     : %d", __func__, font.stretch());
#endif
    setFont(font);
    m_glyphs = vtGlyphs(font, m_font_w, m_font_h);
}

/**
 * @brief Response with the terminal identifier
 *
 * Response depends on the decTerminalID resource setting:
 *
 *  -> CSI ? 1 ; 2 c  ("VT100 with Advanced Video Option")
 *  -> CSI ? 1 ; 0 c  ("VT101 with No Options")
 *  -> CSI ? 6 c  ("VT102")
 *  -> CSI ? 1 2 ; Psc  ("VT125")
 *  -> CSI ? 6 2 ; Psc  ("VT220")
 *  -> CSI ? 6 3 ; Psc  ("VT320")
 *  -> CSI ? 6 4 ; Psc  ("VT420")
 *
 *  The VT100-style response parameters do not mean anything by
 *  themselves.  VT220 (and higher) parameters do, telling the
 *  host what features the terminal supports:
 *    Ps = 1  -> 132-columns.
 *    Ps = 2  -> Printer.
 *    Ps = 3  -> ReGIS graphics.
 *    Ps = 4  -> Sixel graphics.
 *    Ps = 6  -> Selective erase.
 *    Ps = 8  -> User-defined keys.
 *    Ps = 9  -> National Replacement Character sets.
 *    Ps = 1 5  -> Technical characters.
 *    Ps = 1 6  -> Locator port.
 *    Ps = 1 7  -> Terminal state interrogation.
 *    Ps = 1 8  -> User windows.
 *    Ps = 2 1  -> Horizontal scrolling.
 *    Ps = 2 2  -> ANSI color, e.g., VT525.
 *    Ps = 2 8  -> Rectangular editing.
 *    Ps = 2 9  -> ANSI text locator (i.e., DEC Locator mode).
 */
void vt220::vt_DECID(int Ps)
{
    QByteArray response = m_s8c1t ? QByteArray("\x9b") : QByteArray("\x1b[");
    response += QByteArray("?");
    switch (m_terminal) {
    case VT100:
	response += QByteArray("1;2");
	Ps = 0;
	break;
    case VT101:
	response += QByteArray("1;0");
	Ps = 0;
	break;
    case VT102:
	response += QByteArray("6");
	Ps = 0;
	break;
    case VT125:
	response += QByteArray("12;1");
	Ps = 0;
	break;
    case VT200:
	response += QByteArray("62");
	break;
    case VT300:
	response += QByteArray("63") + QByteArray::number(Ps);
	break;
    case VT400:
	response += QByteArray("64") + QByteArray::number(Ps);
	break;
    case VT500:
	response += QByteArray("65") + QByteArray::number(Ps);
	break;
    }

    if (Ps) {
	// TODO: check if we support Ps
	response += QByteArray(";");
	response += QByteArray::number(Ps);
    }
    response += QByteArray("c");
    emit term_response(response);
}

/**
 * @brief Reset the terminal to a Terminal type @p term and @p width columns × @p height rows
 * @param term terminal type
 * @param width number of columns
 * @param height number of rows
 */
void vt220::term_reset(Terminal term, int width, int height)
{
    FUN("term_reset");
    qint32 y;

    m_terminal = term;

    m_charmaps.resize(NRCS_COUNT);
    m_charmap_name = {
	    { MAP_VT500, tr("VT500") },
	    { MAP_INVALID, tr("Invalid") },
	    { MAP_LATIN1, tr("Latin-1") },
	    { MAP_DECGR, tr("DEC graphics") },
	    { MAP_IBMPC, tr("IBM-PC") },
	    { MAP_USER, tr("User defined") },
	    { NRCS_USASCII, tr("(B) US-ASCII") },
	    { NRCS_BRITISH, tr("(A) British") },
	    { NRCS_NORWEGIAN_DANISH, tr("(E) Norwegian / Danish") },
	    { NRCS_FINNISH, tr("(C) Finnish") },
	    { NRCS_SWEDISH, tr("(H) Swedish") },
	    { NRCS_GERMAN, tr("(K) German") },
	    { NRCS_FRENCH_CANADIAN, tr("(Q) French Canadian") },
	    { NRCS_FRENCH, tr("(R) French") },
	    { NRCS_ITALIAN, tr("(Y) Italian") },
	    { NRCS_SPANISH, tr("(Z) Spanish") },
	    { NRCS_DUTCH, tr("(4) Dutch") },
	    { NRCS_SWISS, tr("(=) Swiss") },
	    { NRCS_PORTUGESE, tr("(6) Portugese") },
	    { NRCS_GREEK, tr("(>) Greek") },
	    { NRCS_TURKISH, tr("(2) Turkish") },
	    { NRCS_HEBREW, tr("(=) Hebrew") },
	    { NRCS_COUNT, QLatin1String("<COUNT>") },
	};

    // Latin-1 (ISO-8859-1)
    m_charmaps[MAP_LATIN1] = cmapHash({
	{0x20,' '},  {0x21,'!'},  {0x22,'"'},  {0x23,'#'},  {0x24,'$'},  {0x25,'%'},  {0x26,'&'},  {0x27,'\''}, {0x28,'('},  {0x29,')'},  {0x2a,'*'},  {0x2b,'+'},  {0x2c,','},  {0x2d,'-'},  {0x2e,'.'},  {0x2f,'/'},
	{0x30,'0'},  {0x31,'1'},  {0x32,'2'},  {0x33,'3'},  {0x34,'4'},  {0x35,'5'},  {0x36,'6'},  {0x37,'7'},  {0x38,'8'},  {0x39,'9'},  {0x3a,':'},  {0x3b,';'},  {0x3c,'<'},  {0x3d,'='},  {0x3e,'>'},  {0x3f,'?'},
	{0x40,'@'},  {0x41,'A'},  {0x42,'B'},  {0x43,'C'},  {0x44,'D'},  {0x45,'E'},  {0x46,'F'},  {0x47,'G'},  {0x48,'H'},  {0x49,'I'},  {0x4a,'J'},  {0x4b,'K'},  {0x4c,'L'},  {0x4d,'M'},  {0x4e,'N'},  {0x4f,'O'},
	{0x50,'P'},  {0x51,'Q'},  {0x52,'R'},  {0x53,'S'},  {0x54,'T'},  {0x55,'U'},  {0x56,'V'},  {0x57,'W'},  {0x58,'X'},  {0x59,'Y'},  {0x5a,'Z'},  {0x5b,'['},  {0x5c,'\\'}, {0x5d,']'},  {0x5e,'^'},  {0x5f,'_'},
	{0x60,'`'},  {0x61,'a'},  {0x62,'b'},  {0x63,'c'},  {0x64,'d'},  {0x65,'e'},  {0x66,'f'},  {0x67,'g'},  {0x68,'h'},  {0x69,'i'},  {0x6a,'j'},  {0x6b,'k'},  {0x6c,'l'},  {0x6d,'m'},  {0x6e,'n'},  {0x6f,'o'},
	{0x70,'p'},  {0x71,'q'},  {0x72,'r'},  {0x73,'s'},  {0x74,'t'},  {0x75,'u'},  {0x76,'v'},  {0x77,'w'},  {0x78,'x'},  {0x79,'y'},  {0x7a,'z'},  {0x7b,'{'},  {0x7c,'|'},  {0x7d,'}'},  {0x7e,'~'},  {0x7f,DEL},
    });

    // DEC Special Graphics
    m_charmaps[MAP_DECGR] = cmapHash({
	{0x20,' '},  {0x21,'!'},  {0x22,'"'},  {0x23,'#'},  {0x24,'$'},  {0x25,'%'},  {0x26,'&'},  {0x27,'\''}, {0x28,'('},  {0x29,')'},  {0x2a,'*'},  {0x2b,L'→'}, {0x2c,L'←'}, {0x2d,L'↑'}, {0x2e,L'↓'}, {0x2f,'/'},
	{0x30,'0'},  {0x31,'1'},  {0x32,'2'},  {0x33,'3'},  {0x34,'4'},  {0x35,'5'},  {0x36,'6'},  {0x37,'7'},  {0x38,'8'},  {0x39,'9'},  {0x3a,':'},  {0x3b,';'},  {0x3c,'<'},  {0x3d,'='},  {0x3e,'>'},  {0x3f,'?'},
	{0x40,L'█'}, {0x41,'A'},  {0x42,'B'},  {0x43,'C'},  {0x44,'D'},  {0x45,'E'},  {0x46,'F'},  {0x47,'G'},  {0x48,'H'},  {0x49,'I'},  {0x4a,'J'},  {0x4b,'K'},  {0x4c,'L'},  {0x4d,'M'},  {0x4e,'N'},  {0x4f,'O'},
	{0x50,'P'},  {0x51,'Q'},  {0x52,'R'},  {0x53,'S'},  {0x54,'T'},  {0x55,'U'},  {0x56,'V'},  {0x57,'W'},  {0x58,'X'},  {0x59,'Y'},  {0x5a,'Z'},  {0x5b,'['},  {0x5c,'\\'}, {0x5d,']'},  {0x5e,'^'},  {0x5f,' '},
	{0x60,L'◆'}, {0x61,L'▒'}, {0x62,L'␉'}, {0x63,L'␌'}, {0x64,L'␍'}, {0x65,L'␊'}, {0x66,L'°'}, {0x67,L'±'}, {0x68,L'░'}, {0x69,L'␋'}, {0x6a,L'┘'}, {0x6b,L'┐'}, {0x6c,L'┌'}, {0x6d,L'└'}, {0x6e,L'┼'}, {0x6f,L'⎺'},
	{0x70,L'⎻'}, {0x71,L'─'}, {0x72,L'⎼'}, {0x73,L'⎽'}, {0x74,L'├'}, {0x75,L'┤'}, {0x76,L'┴'}, {0x77,L'┬'}, {0x78,L'│'}, {0x79,L'≤'}, {0x7a,L'≥'}, {0x7b,L'π'}, {0x7c,L'≠'}, {0x7d,L'£'}, {0x7e,L'·'}, {0x7f,L'¸'},
    });

    // Code Page 437 (IBM-PC)
    m_charmaps[MAP_IBMPC] = cmapHash({
	{0x00,NUL},  {0x01,L'☺'}, {0x02,L'☻'}, {0x03,L'♥'}, {0x04,L'♦'}, {0x05,L'♣'}, {0x06,L'♠'}, {0x07,L'•'}, {0x08,L'◘'}, {0x09,L'○'}, {0x0a,L'◙'}, {0x0b,L'♂'}, {0x0c,L'♀'}, {0x0d,L'♪'}, {0x0e,L'♫'}, {0x0f,L'☼'},
	{0x10,L'▶'}, {0x11,L'◀'}, {0x12,L'↕'}, {0x13,L'‼'}, {0x14,L'¶'}, {0x15,L'§'}, {0x16,L'▬'}, {0x17,L'↨'}, {0x18,L'↑'}, {0x19,L'↓'}, {0x1a,L'→'}, {0x1b,L'←'}, {0x1c,L'∟'}, {0x1d,L'↔'}, {0x1e,L'▲'}, {0x1f,L'▼'},
	{0x20,' '},  {0x21,'!'},  {0x22,'"'},  {0x23,'#'},  {0x24,'$'},  {0x25,'%'},  {0x26,'&'},  {0x27,'\''}, {0x28,'('},  {0x29,')'},  {0x2a,'*'},  {0x2b,'+'},  {0x2c,','},  {0x2d,'-'},  {0x2e,'.'},  {0x2f,'/'},
	{0x30,'0'},  {0x31,'1'},  {0x32,'2'},  {0x33,'3'},  {0x34,'4'},  {0x35,'5'},  {0x36,'6'},  {0x37,'7'},  {0x38,'8'},  {0x39,'9'},  {0x3a,':'},  {0x3b,';'},  {0x3c,'<'},  {0x3d,'='},  {0x3e,'>'},  {0x3f,'?'},
	{0x40,'@'},  {0x41,'A'},  {0x42,'B'},  {0x43,'C'},  {0x44,'D'},  {0x45,'E'},  {0x46,'F'},  {0x47,'G'},  {0x48,'H'},  {0x49,'I'},  {0x4a,'J'},  {0x4b,'K'},  {0x4c,'L'},  {0x4d,'M'},  {0x4e,'N'},  {0x4f,'O'},
	{0x50,'P'},  {0x51,'Q'},  {0x52,'R'},  {0x53,'S'},  {0x54,'T'},  {0x55,'U'},  {0x56,'V'},  {0x57,'W'},  {0x58,'X'},  {0x59,'Y'},  {0x5a,'Z'},  {0x5b,'['},  {0x5c,'\\'}, {0x5d,']'},  {0x5e,'^'},  {0x5f,'_'},
	{0x60,'`'},  {0x61,'a'},  {0x62,'b'},  {0x63,'c'},  {0x64,'d'},  {0x65,'e'},  {0x66,'f'},  {0x67,'g'},  {0x68,'h'},  {0x69,'i'},  {0x6a,'j'},  {0x6b,'k'},  {0x6c,'l'},  {0x6d,'m'},  {0x6e,'n'},  {0x6f,'o'},
	{0x70,'p'},  {0x71,'q'},  {0x72,'r'},  {0x73,'s'},  {0x74,'t'},  {0x75,'u'},  {0x76,'v'},  {0x77,'w'},  {0x78,'x'},  {0x79,'y'},  {0x7a,'z'},  {0x7b,'{'},  {0x7c,'|'},  {0x7d,'}'},  {0x7e,'~'},  {0x7f,L'⌂'},
	{0x80,L'Ç'}, {0x81,L'ü'}, {0x82,L'é'}, {0x83,L'â'}, {0x84,L'ä'}, {0x85,L'à'}, {0x86,L'å'}, {0x87,L'ç'}, {0x88,L'ê'}, {0x89,L'ë'}, {0x8a,L'è'}, {0x8b,L'ï'}, {0x8c,L'î'}, {0x8d,L'ì'}, {0x8e,L'Ä'}, {0x8f,L'Å'},
	{0x90,L'É'}, {0x91,L'æ'}, {0x92,L'Æ'}, {0x93,L'ô'}, {0x94,L'ö'}, {0x95,L'ò'}, {0x96,L'û'}, {0x97,L'ù'}, {0x98,L'ÿ'}, {0x99,L'Ö'}, {0x9a,L'Ü'}, {0x9b,L'¢'}, {0x9c,L'£'}, {0x9d,L'¥'}, {0x9e,L'₧'}, {0x9f,L'ƒ'},
	{0xa0,L'á'}, {0xa1,L'í'}, {0xa2,L'ó'}, {0xa3,L'ú'}, {0xa4,L'ñ'}, {0xa5,L'Ñ'}, {0xa6,L'ª'}, {0xa7,L'º'}, {0xa8,L'¿'}, {0xa9,L'⌐'}, {0xaa,L'¬'}, {0xab,L'½'}, {0xac,L'¼'}, {0xad,L'¡'}, {0xae,L'«'}, {0xaf,L'»'},
	{0xb0,L'░'}, {0xb1,L'▒'}, {0xb2,L'▓'}, {0xb3,L'│'}, {0xb4,L'┤'}, {0xb5,L'╡'}, {0xb6,L'╢'}, {0xb7,L'╖'}, {0xb8,L'╕'}, {0xb9,L'╣'}, {0xba,L'║'}, {0xbb,L'╗'}, {0xbc,L'╝'}, {0xbd,L'╜'}, {0xbe,L'╛'}, {0xbf,L'┐'},
	{0xc0,L'└'}, {0xc1,L'┴'}, {0xc2,L'┬'}, {0xc3,L'├'}, {0xc4,L'─'}, {0xc5,L'┼'}, {0xc6,L'╞'}, {0xc7,L'╟'}, {0xc8,L'╚'}, {0xc9,L'╔'}, {0xca,L'╩'}, {0xcb,L'╦'}, {0xcc,L'╠'}, {0xcd,L'═'}, {0xce,L'╬'}, {0xcf,L'╧'},
	{0xd0,L'╨'}, {0xd1,L'╤'}, {0xd2,L'╥'}, {0xd3,L'╙'}, {0xd4,L'╘'}, {0xd5,L'╒'}, {0xd6,L'╓'}, {0xd7,L'╫'}, {0xd8,L'╪'}, {0xd9,L'┘'}, {0xda,L'┌'}, {0xdb,L'█'}, {0xdc,L'▄'}, {0xdd,L'▌'}, {0xde,L'▐'}, {0xdf,L'▀'},
	{0xe0,L'α'}, {0xe1,L'ß'}, {0xe2,L'Γ'}, {0xe3,L'π'}, {0xe4,L'Σ'}, {0xe5,L'σ'}, {0xe6,L'µ'}, {0xe7,L'τ'}, {0xe8,L'Φ'}, {0xe9,L'Θ'}, {0xea,L'Ω'}, {0xeb,L'δ'}, {0xec,L'∞'}, {0xed,L'φ'}, {0xee,L'ε'}, {0xef,L'∩'},
	{0xf0,L'≡'}, {0xf1,L'±'}, {0xf2,L'≥'}, {0xf3,L'≤'}, {0xf4,L'⌠'}, {0xf5,L'⌡'}, {0xf6,L'÷'}, {0xf7,L'≈'}, {0xf8,L'°'}, {0xf9,L'∙'}, {0xfa,L'·'}, {0xfb,L'√'}, {0xfc,L'ⁿ'}, {0xfd,L'²'}, {0xfe,L'■'}, {0xff,' '},
    });

    // User defined
    m_charmaps[MAP_USER] = cmapHash({
	{0x20,' '},  {0x21,L'⁑'}, {0x22,L'⁒'}, {0x23,L'⁓'}, {0x24,L'⁔'}, {0x25,L'⁕'}, {0x26,L'⁖'}, {0x27,L'⁗'}, {0x28,L'⁘'}, {0x29,L'⁙'}, {0x2a,L'⁚'}, {0x2b,L'⁛'}, {0x2c,L'⁜'}, {0x2d,L'⁝'}, {0x2e,L'⁞'}, {0x2f,'/'},
	{0x30,L'·'}, {0x31,L'·'}, {0x32,L'·'}, {0x33,L'·'}, {0x34,L'·'}, {0x35,L'·'}, {0x36,L'·'}, {0x37,L'·'}, {0x38,L'·'}, {0x39,L'·'}, {0x3a,L'·'}, {0x3b,L'·'}, {0x3c,L'·'}, {0x3d,L'·'}, {0x3e,L'·'}, {0x3f,L'·'},
	{0x40,L'·'}, {0x41,L'‐'}, {0x42,L'‑'}, {0x43,L'‒'}, {0x44,L'–'}, {0x45,L'—'}, {0x46,L'―'}, {0x47,L'‖'}, {0x48,L'‗'}, {0x49,L'‘'}, {0x4a,L'’'}, {0x4b,L'‚'}, {0x4c,L'‛'}, {0x4d,L'“'}, {0x4e,L'”'}, {0x4f,L'„'},
	{0x50,L'‟'}, {0x51,L'†'}, {0x52,L'‡'}, {0x53,L'•'}, {0x54,L'‣'}, {0x55,L'․'}, {0x56,L'‥'}, {0x57,L'…'}, {0x58,L'‧'}, {0x59,L'‰'}, {0x5a,L'‱'}, {0x5b,'['},  {0x5c,'\\'}, {0x5d,']'},  {0x5e,'^'},  {0x5f,'_'},
	{0x60,L'′'}, {0x61,L'″'}, {0x62,L'‴'}, {0x63,L'‵'}, {0x64,L'‶'}, {0x65,L'‷'}, {0x66,L'‸'}, {0x67,L'‹'}, {0x68,L'›'}, {0x69,L'※'}, {0x6a,L'‼'}, {0x6b,L'‽'}, {0x6c,L'‾'}, {0x6d,L'‿'}, {0x6e,L'⁀'}, {0x6f,L'⁁'},
	{0x70,L'⁂'}, {0x71,L'⁃'}, {0x72,L'⁄'}, {0x73,L'⁅'}, {0x74,L'⁆'}, {0x75,L'⁇'}, {0x76,L'⁈'}, {0x77,L'⁉'}, {0x78,L'⁊'}, {0x79,L'⁋'}, {0x7a,L'⁌'}, {0x7b,L'⁍'}, {0x7c,L'⁎'}, {0x7d,L'⁏'}, {0x7e,L'⁐'}, {0x7f,DEL},
    });

    // User Defined
    m_charmaps[NRCS_USASCII] = cmapHash({
	{0x20,' '},  {0x21,'!'},  {0x22,'"'},  {0x23,'#'},  {0x24,'$'},  {0x25,'%'},  {0x26,'&'},  {0x27,'\''}, {0x28,'('},  {0x29,')'},  {0x2a,'*'},  {0x2b,'+'},  {0x2c,','},  {0x2d,'-'},  {0x2e,'.'},  {0x2f,'/'},
	{0x30,'0'},  {0x31,'1'},  {0x32,'2'},  {0x33,'3'},  {0x34,'4'},  {0x35,'5'},  {0x36,'6'},  {0x37,'7'},  {0x38,'8'},  {0x39,'9'},  {0x3a,':'},  {0x3b,';'},  {0x3c,'<'},  {0x3d,'='},  {0x3e,'>'},  {0x3f,'?'},
	{0x40,'@'},  {0x41,'A'},  {0x42,'B'},  {0x43,'C'},  {0x44,'D'},  {0x45,'E'},  {0x46,'F'},  {0x47,'G'},  {0x48,'H'},  {0x49,'I'},  {0x4a,'J'},  {0x4b,'K'},  {0x4c,'L'},  {0x4d,'M'},  {0x4e,'N'},  {0x4f,'O'},
	{0x50,'P'},  {0x51,'Q'},  {0x52,'R'},  {0x53,'S'},  {0x54,'T'},  {0x55,'U'},  {0x56,'V'},  {0x57,'W'},  {0x58,'X'},  {0x59,'Y'},  {0x5a,'Z'},  {0x5b,'['},  {0x5c,'\\'}, {0x5d,']'},  {0x5e,'^'},  {0x5f,'_'},
	{0x60,'`'},  {0x61,'a'},  {0x62,'b'},  {0x63,'c'},  {0x64,'d'},  {0x65,'e'},  {0x66,'f'},  {0x67,'g'},  {0x68,'h'},  {0x69,'i'},  {0x6a,'j'},  {0x6b,'k'},  {0x6c,'l'},  {0x6d,'m'},  {0x6e,'n'},  {0x6f,'o'},
	{0x70,'p'},  {0x71,'q'},  {0x72,'r'},  {0x73,'s'},  {0x74,'t'},  {0x75,'u'},  {0x76,'v'},  {0x77,'w'},  {0x78,'x'},  {0x79,'y'},  {0x7a,'z'},  {0x7b,'{'},  {0x7c,'|'},  {0x7d,'}'},  {0x7e,'~'},  {0x7f,DEL},
    });

    // NRCS British
    m_charmaps[NRCS_BRITISH] = cmapHash({
	{0x20,' '},  {0x21,'!'},  {0x22,'"'},  {0x23,L'£'}, {0x24,'$'},  {0x25,'%'},  {0x26,'&'},  {0x27,'\''}, {0x28,'('},  {0x29,')'},  {0x2a,'*'},  {0x2b,'+'},  {0x2c,','},  {0x2d,'-'},  {0x2e,'.'},  {0x2f,'/'},
	{0x30,'0'},  {0x31,'1'},  {0x32,'2'},  {0x33,'3'},  {0x34,'4'},  {0x35,'5'},  {0x36,'6'},  {0x37,'7'},  {0x38,'8'},  {0x39,'9'},  {0x3a,':'},  {0x3b,';'},  {0x3c,'<'},  {0x3d,'='},  {0x3e,'>'},  {0x3f,'?'},
	{0x40,'@'},  {0x41,'A'},  {0x42,'B'},  {0x43,'C'},  {0x44,'D'},  {0x45,'E'},  {0x46,'F'},  {0x47,'G'},  {0x48,'H'},  {0x49,'I'},  {0x4a,'J'},  {0x4b,'K'},  {0x4c,'L'},  {0x4d,'M'},  {0x4e,'N'},  {0x4f,'O'},
	{0x50,'P'},  {0x51,'Q'},  {0x52,'R'},  {0x53,'S'},  {0x54,'T'},  {0x55,'U'},  {0x56,'V'},  {0x57,'W'},  {0x58,'X'},  {0x59,'Y'},  {0x5a,'Z'},  {0x5b,'['},  {0x5c,'\\'}, {0x5d,']'},  {0x5e,'^'},  {0x5f,'_'},
	{0x60,'`'},  {0x61,'a'},  {0x62,'b'},  {0x63,'c'},  {0x64,'d'},  {0x65,'e'},  {0x66,'f'},  {0x67,'g'},  {0x68,'h'},  {0x69,'i'},  {0x6a,'j'},  {0x6b,'k'},  {0x6c,'l'},  {0x6d,'m'},  {0x6e,'n'},  {0x6f,'o'},
	{0x70,'p'},  {0x71,'q'},  {0x72,'r'},  {0x73,'s'},  {0x74,'t'},  {0x75,'u'},  {0x76,'v'},  {0x77,'w'},  {0x78,'x'},  {0x79,'y'},  {0x7a,'z'},  {0x7b,'{'},  {0x7c,'|'},  {0x7d,'}'},  {0x7e,'~'},  {0x7f,DEL},
    });

    // NRCS Norwegian/Danish
    m_charmaps[NRCS_NORWEGIAN_DANISH] = cmapHash({
	{0x20,' '},  {0x21,'!'},  {0x22,'"'},  {0x23,'#'},  {0x24,'$'},  {0x25,'%'},  {0x26,'&'},  {0x27,'\''}, {0x28,'('},  {0x29,')'},  {0x2a,'*'},  {0x2b,'+'},  {0x2c,','},  {0x2d,'-'},  {0x2e,'.'},  {0x2f,'/'},
	{0x30,'0'},  {0x31,'1'},  {0x32,'2'},  {0x33,'3'},  {0x34,'4'},  {0x35,'5'},  {0x36,'6'},  {0x37,'7'},  {0x38,'8'},  {0x39,'9'},  {0x3a,':'},  {0x3b,';'},  {0x3c,'<'},  {0x3d,'='},  {0x3e,'>'},  {0x3f,'?'},
	{0x40,L'Ä'}, {0x41,'A'},  {0x42,'B'},  {0x43,'C'},  {0x44,'D'},  {0x45,'E'},  {0x46,'F'},  {0x47,'G'},  {0x48,'H'},  {0x49,'I'},  {0x4a,'J'},  {0x4b,'K'},  {0x4c,'L'},  {0x4d,'M'},  {0x4e,'N'},  {0x4f,'O'},
	{0x50,'P'},  {0x51,'Q'},  {0x52,'R'},  {0x53,'S'},  {0x54,'T'},  {0x55,'U'},  {0x56,'V'},  {0x57,'W'},  {0x58,'X'},  {0x59,'Y'},  {0x5a,'Z'},  {0x5b,L'Æ'}, {0x5c,L'Ø'}, {0x5d,L'Å'}, {0x5e,'^'},  {0x5f,'_'},
	{0x60,L'Ü'}, {0x61,'a'},  {0x62,'b'},  {0x63,'c'},  {0x64,'d'},  {0x65,'e'},  {0x66,'f'},  {0x67,'g'},  {0x68,'h'},  {0x69,'i'},  {0x6a,'j'},  {0x6b,'k'},  {0x6c,'l'},  {0x6d,'m'},  {0x6e,'n'},  {0x6f,'o'},
	{0x70,'p'},  {0x71,'q'},  {0x72,'r'},  {0x73,'s'},  {0x74,'t'},  {0x75,'u'},  {0x76,'v'},  {0x77,'w'},  {0x78,'x'},  {0x79,'y'},  {0x7a,'z'},  {0x7b,L'æ'}, {0x7c,L'ø'}, {0x7d,L'å'}, {0x7e,L'ü'}, {0x7f,DEL},
    });

    // NRCS Finnish
    m_charmaps[NRCS_FINNISH] = cmapHash({
	{0x20,' '},  {0x21,'!'},  {0x22,'"'},  {0x23,'#'},  {0x24,'$'},  {0x25,'%'},  {0x26,'&'},  {0x27,'\''}, {0x28,'('},  {0x29,')'},  {0x2a,'*'},  {0x2b,'+'},  {0x2c,','},  {0x2d,'-'},  {0x2e,'.'},  {0x2f,'/'},
	{0x30,'0'},  {0x31,'1'},  {0x32,'2'},  {0x33,'3'},  {0x34,'4'},  {0x35,'5'},  {0x36,'6'},  {0x37,'7'},  {0x38,'8'},  {0x39,'9'},  {0x3a,':'},  {0x3b,';'},  {0x3c,'<'},  {0x3d,'='},  {0x3e,'>'},  {0x3f,'?'},
	{0x40,'@'},  {0x41,'A'},  {0x42,'B'},  {0x43,'C'},  {0x44,'D'},  {0x45,'E'},  {0x46,'F'},  {0x47,'G'},  {0x48,'H'},  {0x49,'I'},  {0x4a,'J'},  {0x4b,'K'},  {0x4c,'L'},  {0x4d,'M'},  {0x4e,'N'},  {0x4f,'O'},
	{0x50,'P'},  {0x51,'Q'},  {0x52,'R'},  {0x53,'S'},  {0x54,'T'},  {0x55,'U'},  {0x56,'V'},  {0x57,'W'},  {0x58,'X'},  {0x59,'Y'},  {0x5a,'Z'},  {0x5b,L'Ä'}, {0x5c,L'Ö'}, {0x5d,L'Å'}, {0x5e,'^'},  {0x5f,'_'},
	{0x60,L'Ü'}, {0x61,'a'},  {0x62,'b'},  {0x63,'c'},  {0x64,'d'},  {0x65,'e'},  {0x66,'f'},  {0x67,'g'},  {0x68,'h'},  {0x69,'i'},  {0x6a,'j'},  {0x6b,'k'},  {0x6c,'l'},  {0x6d,'m'},  {0x6e,'n'},  {0x6f,'o'},
	{0x70,'p'},  {0x71,'q'},  {0x72,'r'},  {0x73,'s'},  {0x74,'t'},  {0x75,'u'},  {0x76,'v'},  {0x77,'w'},  {0x78,'x'},  {0x79,'y'},  {0x7a,'z'},  {0x7b,L'ä'}, {0x7c,L'ö'}, {0x7d,L'å'}, {0x7e,L'ü'}, {0x7f,DEL},
    });

    // NRCS Swedish
    m_charmaps[NRCS_SWEDISH] = cmapHash({
	{0x20,' '},  {0x21,'!'},  {0x22,'"'},  {0x23,'#'},  {0x24,'$'},  {0x25,'%'},  {0x26,'&'},  {0x27,'\''}, {0x28,'('},  {0x29,')'},  {0x2a,'*'},  {0x2b,'+'},  {0x2c,','},  {0x2d,'-'},  {0x2e,'.'},  {0x2f,'/'},
	{0x30,'0'},  {0x31,'1'},  {0x32,'2'},  {0x33,'3'},  {0x34,'4'},  {0x35,'5'},  {0x36,'6'},  {0x37,'7'},  {0x38,'8'},  {0x39,'9'},  {0x3a,':'},  {0x3b,';'},  {0x3c,'<'},  {0x3d,'='},  {0x3e,'>'},  {0x3f,'?'},
	{0x40,L'É'}, {0x41,'A'},  {0x42,'B'},  {0x43,'C'},  {0x44,'D'},  {0x45,'E'},  {0x46,'F'},  {0x47,'G'},  {0x48,'H'},  {0x49,'I'},  {0x4a,'J'},  {0x4b,'K'},  {0x4c,'L'},  {0x4d,'M'},  {0x4e,'N'},  {0x4f,'O'},
	{0x50,'P'},  {0x51,'Q'},  {0x52,'R'},  {0x53,'S'},  {0x54,'T'},  {0x55,'U'},  {0x56,'V'},  {0x57,'W'},  {0x58,'X'},  {0x59,'Y'},  {0x5a,'Z'},  {0x5b,L'Ä'}, {0x5c,L'Ö'}, {0x5d,L'Å'}, {0x5e,'^'},  {0x5f,'_'},
	{0x60,L'é'}, {0x61,'a'},  {0x62,'b'},  {0x63,'c'},  {0x64,'d'},  {0x65,'e'},  {0x66,'f'},  {0x67,'g'},  {0x68,'h'},  {0x69,'i'},  {0x6a,'j'},  {0x6b,'k'},  {0x6c,'l'},  {0x6d,'m'},  {0x6e,'n'},  {0x6f,'o'},
	{0x70,'p'},  {0x71,'q'},  {0x72,'r'},  {0x73,'s'},  {0x74,'t'},  {0x75,'u'},  {0x76,'v'},  {0x77,'w'},  {0x78,'x'},  {0x79,'y'},  {0x7a,'z'},  {0x7b,L'ä'}, {0x7c,L'ö'}, {0x7d,L'å'}, {0x7e,L'ü'}, {0x7f,DEL},
    });

    // NRCS German
    m_charmaps[NRCS_GERMAN] = cmapHash({
	{0x20,' '},  {0x21,'!'},  {0x22,'"'},  {0x23,'#'},  {0x24,'$'},  {0x25,'%'},  {0x26,'&'},  {0x27,'\''}, {0x28,'('},  {0x29,')'},  {0x2a,'*'},  {0x2b,'+'},  {0x2c,','},  {0x2d,'-'},  {0x2e,'.'},  {0x2f,'/'},
	{0x30,'0'},  {0x31,'1'},  {0x32,'2'},  {0x33,'3'},  {0x34,'4'},  {0x35,'5'},  {0x36,'6'},  {0x37,'7'},  {0x38,'8'},  {0x39,'9'},  {0x3a,':'},  {0x3b,';'},  {0x3c,'<'},  {0x3d,'='},  {0x3e,'>'},  {0x3f,'?'},
	{0x40,'@'},  {0x41,'A'},  {0x42,'B'},  {0x43,'C'},  {0x44,'D'},  {0x45,'E'},  {0x46,'F'},  {0x47,'G'},  {0x48,'H'},  {0x49,'I'},  {0x4a,'J'},  {0x4b,'K'},  {0x4c,'L'},  {0x4d,'M'},  {0x4e,'N'},  {0x4f,'O'},
	{0x50,'P'},  {0x51,'Q'},  {0x52,'R'},  {0x53,'S'},  {0x54,'T'},  {0x55,'U'},  {0x56,'V'},  {0x57,'W'},  {0x58,'X'},  {0x59,'Y'},  {0x5a,'Z'},  {0x5b,L'Ä'}, {0x5c,L'Ö'}, {0x5d,L'Ü'}, {0x5e,'^'},  {0x5f,'_'},
	{0x60,'`'},  {0x61,'a'},  {0x62,'b'},  {0x63,'c'},  {0x64,'d'},  {0x65,'e'},  {0x66,'f'},  {0x67,'g'},  {0x68,'h'},  {0x69,'i'},  {0x6a,'j'},  {0x6b,'k'},  {0x6c,'l'},  {0x6d,'m'},  {0x6e,'n'},  {0x6f,'o'},
	{0x70,'p'},  {0x71,'q'},  {0x72,'r'},  {0x73,'s'},  {0x74,'t'},  {0x75,'u'},  {0x76,'v'},  {0x77,'w'},  {0x78,'x'},  {0x79,'y'},  {0x7a,'z'},  {0x7b,L'ä'}, {0x7c,L'ö'}, {0x7d,L'ü'}, {0x7e,L'ß'}, {0x7f,DEL},
    });

    // NRCS French Canadian
    m_charmaps[NRCS_FRENCH_CANADIAN] = cmapHash({
	{0x20,' '},  {0x21,'!'},  {0x22,'"'},  {0x23,L'£'}, {0x24,'$'},  {0x25,'%'},  {0x26,'&'},  {0x27,'\''}, {0x28,'('},  {0x29,')'},  {0x2a,'*'},  {0x2b,'+'},  {0x2c,','},  {0x2d,'-'},  {0x2e,'.'},  {0x2f,'/'},
	{0x30,'0'},  {0x31,'1'},  {0x32,'2'},  {0x33,'3'},  {0x34,'4'},  {0x35,'5'},  {0x36,'6'},  {0x37,'7'},  {0x38,'8'},  {0x39,'9'},  {0x3a,':'},  {0x3b,';'},  {0x3c,'<'},  {0x3d,'='},  {0x3e,'>'},  {0x3f,'?'},
	{0x40,L'à'}, {0x41,'A'},  {0x42,'B'},  {0x43,'C'},  {0x44,'D'},  {0x45,'E'},  {0x46,'F'},  {0x47,'G'},  {0x48,'H'},  {0x49,'I'},  {0x4a,'J'},  {0x4b,'K'},  {0x4c,'L'},  {0x4d,'M'},  {0x4e,'N'},  {0x4f,'O'},
	{0x50,'P'},  {0x51,'Q'},  {0x52,'R'},  {0x53,'S'},  {0x54,'T'},  {0x55,'U'},  {0x56,'V'},  {0x57,'W'},  {0x58,'X'},  {0x59,'Y'},  {0x5a,'Z'},  {0x5b,L'â'}, {0x5c,L'ç'}, {0x5d,L'ê'}, {0x5e,L'î'}, {0x5f,'_'},
	{0x60,L'ô'}, {0x61,'a'},  {0x62,'b'},  {0x63,'c'},  {0x64,'d'},  {0x65,'e'},  {0x66,'f'},  {0x67,'g'},  {0x68,'h'},  {0x69,'i'},  {0x6a,'j'},  {0x6b,'k'},  {0x6c,'l'},  {0x6d,'m'},  {0x6e,'n'},  {0x6f,'o'},
	{0x70,'p'},  {0x71,'q'},  {0x72,'r'},  {0x73,'s'},  {0x74,'t'},  {0x75,'u'},  {0x76,'v'},  {0x77,'w'},  {0x78,'x'},  {0x79,'y'},  {0x7a,'z'},  {0x7b,L'é'}, {0x7c,L'ù'}, {0x7d,L'è'}, {0x7e,L'û'}, {0x7f,DEL},
    });

    // NRCS French
    m_charmaps[NRCS_FRENCH] = cmapHash({
	{0x20,' '},  {0x21,'!'},  {0x22,'"'},  {0x23,L'£'}, {0x24,'$'},  {0x25,'%'},  {0x26,'&'},  {0x27,'\''}, {0x28,'('},  {0x29,')'},  {0x2a,'*'},  {0x2b,'+'},  {0x2c,','},  {0x2d,'-'},  {0x2e,'.'},  {0x2f,'/'},
	{0x30,'0'},  {0x31,'1'},  {0x32,'2'},  {0x33,'3'},  {0x34,'4'},  {0x35,'5'},  {0x36,'6'},  {0x37,'7'},  {0x38,'8'},  {0x39,'9'},  {0x3a,':'},  {0x3b,';'},  {0x3c,'<'},  {0x3d,'='},  {0x3e,'>'},  {0x3f,'?'},
	{0x40,L'à'}, {0x41,'A'},  {0x42,'B'},  {0x43,'C'},  {0x44,'D'},  {0x45,'E'},  {0x46,'F'},  {0x47,'G'},  {0x48,'H'},  {0x49,'I'},  {0x4a,'J'},  {0x4b,'K'},  {0x4c,'L'},  {0x4d,'M'},  {0x4e,'N'},  {0x4f,'O'},
	{0x50,'P'},  {0x51,'Q'},  {0x52,'R'},  {0x53,'S'},  {0x54,'T'},  {0x55,'U'},  {0x56,'V'},  {0x57,'W'},  {0x58,'X'},  {0x59,'Y'},  {0x5a,'Z'},  {0x5b,L'°'}, {0x5c,L'ç'}, {0x5d,L'§'}, {0x5e,'^'}, {0x5f,'_'},
	{0x60,'`'},  {0x61,'a'},  {0x62,'b'},  {0x63,'c'},  {0x64,'d'},  {0x65,'e'},  {0x66,'f'},  {0x67,'g'},  {0x68,'h'},  {0x69,'i'},  {0x6a,'j'},  {0x6b,'k'},  {0x6c,'l'},  {0x6d,'m'},  {0x6e,'n'},  {0x6f,'o'},
	{0x70,'p'},  {0x71,'q'},  {0x72,'r'},  {0x73,'s'},  {0x74,'t'},  {0x75,'u'},  {0x76,'v'},  {0x77,'w'},  {0x78,'x'},  {0x79,'y'},  {0x7a,'z'},  {0x7b,L'é'}, {0x7c,L'ù'}, {0x7d,L'è'}, {0x7e,L'¨'}, {0x7f,DEL},
    });

    // NRCS Italian
    m_charmaps[NRCS_ITALIAN] = cmapHash({
	{0x20,' '},  {0x21,'!'},  {0x22,'"'},  {0x23,L'£'}, {0x24,'$'},  {0x25,'%'},  {0x26,'&'},  {0x27,'\''}, {0x28,'('},  {0x29,')'},  {0x2a,'*'},  {0x2b,'+'},  {0x2c,','},  {0x2d,'-'},  {0x2e,'.'},  {0x2f,'/'},
	{0x30,'0'},  {0x31,'1'},  {0x32,'2'},  {0x33,'3'},  {0x34,'4'},  {0x35,'5'},  {0x36,'6'},  {0x37,'7'},  {0x38,'8'},  {0x39,'9'},  {0x3a,':'},  {0x3b,';'},  {0x3c,'<'},  {0x3d,'='},  {0x3e,'>'},  {0x3f,'?'},
	{0x40,L'§'}, {0x41,'A'},  {0x42,'B'},  {0x43,'C'},  {0x44,'D'},  {0x45,'E'},  {0x46,'F'},  {0x47,'G'},  {0x48,'H'},  {0x49,'I'},  {0x4a,'J'},  {0x4b,'K'},  {0x4c,'L'},  {0x4d,'M'},  {0x4e,'N'},  {0x4f,'O'},
	{0x50,'P'},  {0x51,'Q'},  {0x52,'R'},  {0x53,'S'},  {0x54,'T'},  {0x55,'U'},  {0x56,'V'},  {0x57,'W'},  {0x58,'X'},  {0x59,'Y'},  {0x5a,'Z'},  {0x5b,L'°'}, {0x5c,L'ç'}, {0x5d,L'é'}, {0x5e,'^'}, {0x5f,'_'},
	{0x60,L'ù'}, {0x61,'a'},  {0x62,'b'},  {0x63,'c'},  {0x64,'d'},  {0x65,'e'},  {0x66,'f'},  {0x67,'g'},  {0x68,'h'},  {0x69,'i'},  {0x6a,'j'},  {0x6b,'k'},  {0x6c,'l'},  {0x6d,'m'},  {0x6e,'n'},  {0x6f,'o'},
	{0x70,'p'},  {0x71,'q'},  {0x72,'r'},  {0x73,'s'},  {0x74,'t'},  {0x75,'u'},  {0x76,'v'},  {0x77,'w'},  {0x78,'x'},  {0x79,'y'},  {0x7a,'z'},  {0x7b,L'à'}, {0x7c,L'ò'}, {0x7d,L'è'}, {0x7e,L'ì'}, {0x7f,DEL},
    });

    // NRCS Spanish
    m_charmaps[NRCS_SPANISH] = cmapHash({
	{0x20,' '},  {0x21,'!'},  {0x22,'"'},  {0x23,L'£'}, {0x24,'$'},  {0x25,'%'},  {0x26,'&'},  {0x27,'\''}, {0x28,'('},  {0x29,')'},  {0x2a,'*'},  {0x2b,'+'},  {0x2c,','},  {0x2d,'-'},  {0x2e,'.'},  {0x2f,'/'},
	{0x30,'0'},  {0x31,'1'},  {0x32,'2'},  {0x33,'3'},  {0x34,'4'},  {0x35,'5'},  {0x36,'6'},  {0x37,'7'},  {0x38,'8'},  {0x39,'9'},  {0x3a,':'},  {0x3b,';'},  {0x3c,'<'},  {0x3d,'='},  {0x3e,'>'},  {0x3f,'?'},
	{0x40,L'§'}, {0x41,'A'},  {0x42,'B'},  {0x43,'C'},  {0x44,'D'},  {0x45,'E'},  {0x46,'F'},  {0x47,'G'},  {0x48,'H'},  {0x49,'I'},  {0x4a,'J'},  {0x4b,'K'},  {0x4c,'L'},  {0x4d,'M'},  {0x4e,'N'},  {0x4f,'O'},
	{0x50,'P'},  {0x51,'Q'},  {0x52,'R'},  {0x53,'S'},  {0x54,'T'},  {0x55,'U'},  {0x56,'V'},  {0x57,'W'},  {0x58,'X'},  {0x59,'Y'},  {0x5a,'Z'},  {0x5b,L'¡'}, {0x5c,L'Ñ'}, {0x5d,L'¿'}, {0x5e,'^'},  {0x5f,'_'},
	{0x60,'`'},  {0x61,'a'},  {0x62,'b'},  {0x63,'c'},  {0x64,'d'},  {0x65,'e'},  {0x66,'f'},  {0x67,'g'},  {0x68,'h'},  {0x69,'i'},  {0x6a,'j'},  {0x6b,'k'},  {0x6c,'l'},  {0x6d,'m'},  {0x6e,'n'},  {0x6f,'o'},
	{0x70,'p'},  {0x71,'q'},  {0x72,'r'},  {0x73,'s'},  {0x74,'t'},  {0x75,'u'},  {0x76,'v'},  {0x77,'w'},  {0x78,'x'},  {0x79,'y'},  {0x7a,'z'},  {0x7b,L'°'}, {0x7c,L'ñ'}, {0x7d,L'ç'}, {0x7e,'~'},  {0x7f,DEL},
    });

    // NRCS Dutch
    m_charmaps[NRCS_DUTCH] = cmapHash({
	{0x20,' '},  {0x21,'!'},  {0x22,'"'},  {0x23,L'£'}, {0x24,'$'},  {0x25,'%'},  {0x26,'&'},  {0x27,'\''}, {0x28,'('},  {0x29,')'},  {0x2a,'*'},  {0x2b,'+'},  {0x2c,','},  {0x2d,'-'},  {0x2e,'.'},  {0x2f,'/'},
	{0x30,'0'},  {0x31,'1'},  {0x32,'2'},  {0x33,'3'},  {0x34,'4'},  {0x35,'5'},  {0x36,'6'},  {0x37,'7'},  {0x38,'8'},  {0x39,'9'},  {0x3a,':'},  {0x3b,';'},  {0x3c,'<'},  {0x3d,'='},  {0x3e,'>'},  {0x3f,'?'},
	{0x40,L'¾'}, {0x41,'A'},  {0x42,'B'},  {0x43,'C'},  {0x44,'D'},  {0x45,'E'},  {0x46,'F'},  {0x47,'G'},  {0x48,'H'},  {0x49,'I'},  {0x4a,'J'},  {0x4b,'K'},  {0x4c,'L'},  {0x4d,'M'},  {0x4e,'N'},  {0x4f,'O'},
	{0x50,'P'},  {0x51,'Q'},  {0x52,'R'},  {0x53,'S'},  {0x54,'T'},  {0x55,'U'},  {0x56,'V'},  {0x57,'W'},  {0x58,'X'},  {0x59,'Y'},  {0x5a,'Z'},  {0x5b,L'ĳ'}, {0x5c,L'½'}, {0x5d,L'|'}, {0x5e,'^'},  {0x5f,'_'},
	{0x60,L'ù'}, {0x61,'a'},  {0x62,'b'},  {0x63,'c'},  {0x64,'d'},  {0x65,'e'},  {0x66,'f'},  {0x67,'g'},  {0x68,'h'},  {0x69,'i'},  {0x6a,'j'},  {0x6b,'k'},  {0x6c,'l'},  {0x6d,'m'},  {0x6e,'n'},  {0x6f,'o'},
	{0x70,'p'},  {0x71,'q'},  {0x72,'r'},  {0x73,'s'},  {0x74,'t'},  {0x75,'u'},  {0x76,'v'},  {0x77,'w'},  {0x78,'x'},  {0x79,'y'},  {0x7a,'z'},  {0x7b,L'¨'}, {0x7c,L'ƒ'}, {0x7d,L'¼'}, {0x7e,L'´'}, {0x7f,DEL},
    });

    // NRCS Swiss
    m_charmaps[NRCS_SWISS] = cmapHash({
	{0x20,' '},  {0x21,'!'},  {0x22,'"'},  {0x23,L'ù'}, {0x24,'$'},  {0x25,'%'},  {0x26,'&'},  {0x27,'\''}, {0x28,'('},  {0x29,')'},  {0x2a,'*'},  {0x2b,'+'},  {0x2c,','},  {0x2d,'-'},  {0x2e,'.'},  {0x2f,'/'},
	{0x30,'0'},  {0x31,'1'},  {0x32,'2'},  {0x33,'3'},  {0x34,'4'},  {0x35,'5'},  {0x36,'6'},  {0x37,'7'},  {0x38,'8'},  {0x39,'9'},  {0x3a,':'},  {0x3b,';'},  {0x3c,'<'},  {0x3d,'='},  {0x3e,'>'},  {0x3f,'?'},
	{0x40,L'à'}, {0x41,'A'},  {0x42,'B'},  {0x43,'C'},  {0x44,'D'},  {0x45,'E'},  {0x46,'F'},  {0x47,'G'},  {0x48,'H'},  {0x49,'I'},  {0x4a,'J'},  {0x4b,'K'},  {0x4c,'L'},  {0x4d,'M'},  {0x4e,'N'},  {0x4f,'O'},
	{0x50,'P'},  {0x51,'Q'},  {0x52,'R'},  {0x53,'S'},  {0x54,'T'},  {0x55,'U'},  {0x56,'V'},  {0x57,'W'},  {0x58,'X'},  {0x59,'Y'},  {0x5a,'Z'},  {0x5b,L'é'}, {0x5c,L'ç'}, {0x5d,L'ê'}, {0x5e,L'î'}, {0x5f,'_'},
	{0x60,L'ô'}, {0x61,'a'},  {0x62,'b'},  {0x63,'c'},  {0x64,'d'},  {0x65,'e'},  {0x66,'f'},  {0x67,'g'},  {0x68,'h'},  {0x69,'i'},  {0x6a,'j'},  {0x6b,'k'},  {0x6c,'l'},  {0x6d,'m'},  {0x6e,'n'},  {0x6f,'o'},
	{0x70,'p'},  {0x71,'q'},  {0x72,'r'},  {0x73,'s'},  {0x74,'t'},  {0x75,'u'},  {0x76,'v'},  {0x77,'w'},  {0x78,'x'},  {0x79,'y'},  {0x7a,'z'},  {0x7b,L'ä'}, {0x7c,L'ö'}, {0x7d,L'ü'}, {0x7e,L'û'}, {0x7f,DEL},
    });

    // NRCS Greek
    m_charmaps[NRCS_GREEK] = cmapHash({
	{0x20,' '},  {0x21,'!'},  {0x22,'"'},  {0x23,L'ù'}, {0x24,'$'},  {0x25,'%'},  {0x26,'&'},  {0x27,'\''}, {0x28,'('},  {0x29,')'},  {0x2a,'*'},  {0x2b,'+'},  {0x2c,','},  {0x2d,'-'},  {0x2e,'.'},  {0x2f,'/'},
	{0x30,'0'},  {0x31,'1'},  {0x32,'2'},  {0x33,'3'},  {0x34,'4'},  {0x35,'5'},  {0x36,'6'},  {0x37,'7'},  {0x38,'8'},  {0x39,'9'},  {0x3a,':'},  {0x3b,';'},  {0x3c,'<'},  {0x3d,'='},  {0x3e,'>'},  {0x3f,'?'},
	{0x40,'@'},  {0x41,L'Α'}, {0x42,L'Β'}, {0x43,L'Γ'}, {0x44,L'Δ'}, {0x45,L'Ε'}, {0x46,L'Ζ'}, {0x47,L'Η'}, {0x48,L'Θ'}, {0x49,L'Ι'}, {0x4a,L'Κ'}, {0x4b,L'Λ'}, {0x4c,L'Μ'}, {0x4d,L'Ν'}, {0x4e,L'Ξ'}, {0x4f,L'Ο'},
	{0x50,L'Π'}, {0x51,L'Ρ'}, {0x52,L'Σ'}, {0x53,L'Τ'}, {0x54,L'Υ'}, {0x55,L'Φ'}, {0x56,L'Χ'}, {0x57,L'Ψ'}, {0x58,L'Ω'}, {0x59,L'Ϊ'}, {0x5a,L'Ϋ'}, {0x5b,'['},  {0x5c,'\\'}, {0x5d,']'},  {0x5e,'^'},  {0x5f,'_'},
	{0x60,'`'},  {0x61,'a'},  {0x62,'b'},  {0x63,'c'},  {0x64,'d'},  {0x65,'e'},  {0x66,'f'},  {0x67,'g'},  {0x68,'h'},  {0x69,'i'},  {0x6a,'j'},  {0x6b,'k'},  {0x6c,'l'},  {0x6d,'m'},  {0x6e,'n'},  {0x6f,'o'},
	{0x70,'p'},  {0x71,'q'},  {0x72,'r'},  {0x73,'s'},  {0x74,'t'},  {0x75,'u'},  {0x76,'v'},  {0x77,'w'},  {0x78,'x'},  {0x79,'y'},  {0x7a,'z'},  {0x7b,L'ä'}, {0x7c,L'ö'}, {0x7d,L'ü'}, {0x7e,L'û'}, {0x7f,DEL},
    });

    // NRCS Portugese
    m_charmaps[NRCS_PORTUGESE] = cmapHash({
	{0x20,' '},  {0x21,'!'},  {0x22,'"'},  {0x23,L'ù'}, {0x24,'$'},  {0x25,'%'},  {0x26,'&'},  {0x27,'\''}, {0x28,'('},  {0x29,')'},  {0x2a,'*'},  {0x2b,'+'},  {0x2c,','},  {0x2d,'-'},  {0x2e,'.'},  {0x2f,'/'},
	{0x30,'0'},  {0x31,'1'},  {0x32,'2'},  {0x33,'3'},  {0x34,'4'},  {0x35,'5'},  {0x36,'6'},  {0x37,'7'},  {0x38,'8'},  {0x39,'9'},  {0x3a,':'},  {0x3b,';'},  {0x3c,'<'},  {0x3d,'='},  {0x3e,'>'},  {0x3f,'?'},
	{0x40,'@'},  {0x41,'A'},  {0x42,'B'},  {0x43,'C'},  {0x44,'D'},  {0x45,'E'},  {0x46,'F'},  {0x47,'G'},  {0x48,'H'},  {0x49,'I'},  {0x4a,'J'},  {0x4b,'K'},  {0x4c,'L'},  {0x4d,'M'},  {0x4e,'N'},  {0x4f,'O'},
	{0x50,'P'},  {0x51,'Q'},  {0x52,'R'},  {0x53,'S'},  {0x54,'T'},  {0x55,'U'},  {0x56,'V'},  {0x57,'W'},  {0x58,'X'},  {0x59,'Y'},  {0x5a,'Z'},  {0x5b,L'Ã'}, {0x5c,L'Ç'}, {0x5d,L'Õ'}, {0x5e,'^'},  {0x5f,'_'},
	{0x60,'`'},  {0x61,'a'},  {0x62,'b'},  {0x63,'c'},  {0x64,'d'},  {0x65,'e'},  {0x66,'f'},  {0x67,'g'},  {0x68,'h'},  {0x69,'i'},  {0x6a,'j'},  {0x6b,'k'},  {0x6c,'l'},  {0x6d,'m'},  {0x6e,'n'},  {0x6f,'o'},
	{0x70,'p'},  {0x71,'q'},  {0x72,'r'},  {0x73,'s'},  {0x74,'t'},  {0x75,'u'},  {0x76,'v'},  {0x77,'w'},  {0x78,'x'},  {0x79,'y'},  {0x7a,'z'},  {0x7b,L'ã'}, {0x7c,L'ç'}, {0x7d,L'õ'}, {0x7e,'~'},  {0x7f,DEL},
    });

    // NRCS Turkish
    m_charmaps[NRCS_TURKISH] = cmapHash({
	{0x20,' '},  {0x21,'!'},  {0x22,'"'},  {0x23,L'ğ'}, {0x24,'$'},  {0x25,'%'},  {0x26,'&'},  {0x27,'\''}, {0x28,'('},  {0x29,')'},  {0x2a,'*'},  {0x2b,'+'},  {0x2c,','},  {0x2d,'-'},  {0x2e,'.'},  {0x2f,'/'},
	{0x30,'0'},  {0x31,'1'},  {0x32,'2'},  {0x33,'3'},  {0x34,'4'},  {0x35,'5'},  {0x36,'6'},  {0x37,'7'},  {0x38,'8'},  {0x39,'9'},  {0x3a,':'},  {0x3b,';'},  {0x3c,'<'},  {0x3d,'='},  {0x3e,'>'},  {0x3f,'?'},
	{0x40,L'İ'}, {0x41,'A'},  {0x42,'B'},  {0x43,'C'},  {0x44,'D'},  {0x45,'E'},  {0x46,'F'},  {0x47,'G'},  {0x48,'H'},  {0x49,'I'},  {0x4a,'J'},  {0x4b,'K'},  {0x4c,'L'},  {0x4d,'M'},  {0x4e,'N'},  {0x4f,'O'},
	{0x50,'P'},  {0x51,'Q'},  {0x52,'R'},  {0x53,'S'},  {0x54,'T'},  {0x55,'U'},  {0x56,'V'},  {0x57,'W'},  {0x58,'X'},  {0x59,'Y'},  {0x5a,'Z'},  {0x5b,L'Ş'}, {0x5c,L'Ö'}, {0x5d,L'Ç'}, {0x5e,'^'},  {0x5f,'_'},
	{0x60,L'Ğ'}, {0x61,'a'},  {0x62,'b'},  {0x63,'c'},  {0x64,'d'},  {0x65,'e'},  {0x66,'f'},  {0x67,'g'},  {0x68,'h'},  {0x69,'i'},  {0x6a,'j'},  {0x6b,'k'},  {0x6c,'l'},  {0x6d,'m'},  {0x6e,'n'},  {0x6f,'o'},
	{0x70,'p'},  {0x71,'q'},  {0x72,'r'},  {0x73,'s'},  {0x74,'t'},  {0x75,'u'},  {0x76,'v'},  {0x77,'w'},  {0x78,'x'},  {0x79,'y'},  {0x7a,'z'},  {0x7b,L'ş'}, {0x7c,L'ö'}, {0x7d,L'ç'}, {0x7e,L'ü'}, {0x7f,DEL},
    });

    // NRCS Hebrew
    m_charmaps[NRCS_HEBREW] = cmapHash({
	{0x20,' '},  {0x21,'!'},  {0x22,'"'},  {0x23,L'£'}, {0x24,'$'},  {0x25,'%'},  {0x26,'&'},  {0x27,'\''}, {0x28,'('},  {0x29,')'},  {0x2a,'*'},  {0x2b,'+'},  {0x2c,','},  {0x2d,'-'},  {0x2e,'.'},  {0x2f,'/'},
	{0x30,'0'},  {0x31,'1'},  {0x32,'2'},  {0x33,'3'},  {0x34,'4'},  {0x35,'5'},  {0x36,'6'},  {0x37,'7'},  {0x38,'8'},  {0x39,'9'},  {0x3a,':'},  {0x3b,';'},  {0x3c,'<'},  {0x3d,'='},  {0x3e,'>'},  {0x3f,'?'},
	{0x40,'@'},  {0x41,'A'},  {0x42,'B'},  {0x43,'C'},  {0x44,'D'},  {0x45,'E'},  {0x46,'F'},  {0x47,'G'},  {0x48,'H'},  {0x49,'I'},  {0x4a,'J'},  {0x4b,'K'},  {0x4c,'L'},  {0x4d,'M'},  {0x4e,'N'},  {0x4f,'O'},
	{0x50,'P'},  {0x51,'Q'},  {0x52,'R'},  {0x53,'S'},  {0x54,'T'},  {0x55,'U'},  {0x56,'V'},  {0x57,'W'},  {0x58,'X'},  {0x59,'Y'},  {0x5a,'Z'},  {0x5b,'['},  {0x5c,'\\'}, {0x5d,']'},  {0x5e,'^'},  {0x5f,'_'},
	{0x60,L'א'}, {0x61,L'ב'}, {0x62,L'ג'}, {0x63,L'ד'}, {0x64,L'ה'}, {0x65,L'ו'}, {0x66,L'ז'}, {0x67,L'ח'}, {0x68,L'ט'}, {0x69,L'י'}, {0x6a,L'ך'}, {0x6b,L'כ'}, {0x6c,L'ל'}, {0x6d,L'ם'}, {0x6e,L'מ'}, {0x6f,L'ן'},
	{0x70,L'נ'}, {0x71,L'ס'}, {0x72,L'ע'}, {0x73,L'ף'}, {0x74,L'פ'}, {0x75,L'ץ'}, {0x76,L'צ'}, {0x77,L'ק'}, {0x78,L'ר'}, {0x79,L'ש'}, {0x7a,L'ת'}, {0x7b,'{'},  {0x7c,'|'},  {0x7d,'}'},  {0x7e,'~'},  {0x7f,DEL},
    });

    m_gmaps.resize(4);
    m_gmaps[0] = m_charmaps[MAP_LATIN1];
    m_gmaps[1] = m_charmaps[MAP_DECGR];
    m_gmaps[2] = m_charmaps[MAP_IBMPC];
    m_gmaps[3] = m_charmaps[MAP_USER];

    m_width = width;
    m_height = height;

    m_top = 0;
    m_bottom = m_height;

    vt_palette_reset();
    vt_tabstop_reset();

    m_cc_mask = 0x7700;
    m_cc_save = m_cc_mask;

    m_csi_args.clear();
    m_state = ESnormal;
    m_ques = false;
    m_decscnm = false;
    m_togmeta = false;
    m_deccm = true;
    m_decim = false;
    m_decom = false;
    m_deccr = true;
    m_decckm = false;
    m_decawm = true;
    m_decarm = true;
    m_repmouse = false;
    m_dspctrl = true;
    m_uc = 7;
    m_hc = 0;
    m_bell_pitch = 750;
    m_bell_duration = 125;
    m_blank_time = 10 * 60;
    m_vesa_time = 10 * 60;
    m_trans = m_gmaps[MAP_LATIN1];
    m_shift = -1;
    m_utf_mode = true;
    m_utf_more = 0;
    m_utf_code = 0;
    m_utf_code_min = 0;

    m_def.set_code(32);
    m_def.set_mark(0);
    m_def.set_fgcolor(C_WHT);
    m_def.set_bgcolor(C_BLK);
    m_def.set_charset(0);
    m_def.set_gl(MAP_LATIN1);
    m_def.set_gr(MAP_DECGR);
    m_def.set_bold(false);
    m_def.set_faint(false);
    m_def.set_italic(false);
    m_def.set_inverse(false);
    m_def.set_underline(false);
    m_def.set_blink(false);
    m_def.set_conceal(false);
    m_def.set_crossed(false);
    m_def.set_underldbl(false);
    m_att = m_def;

    // allocate the screen buffer
    m_backlog.clear();
    m_screen.clear();
    for (y = 0; y < height; y++) {
	m_screen += vtLine(width, m_def);
    }

    m_cursor.x = 0;
    m_cursor.y = 0;
    m_cursor.newx = 0;
    m_cursor.phase = 0;
    m_cursor.on = false;

    update(0, 0, m_width * m_font_w, m_height * m_font_h);
    emit UpdateSize();
}

void vt220::term_set_size(int width, int height)
{
    if (width <= 0)
	width = m_deccolm;
    if (height <= 0)
	height = 25;
    for (int y = 0; y < m_height; y++) {
	vtLine& line = m_screen[y];
	line.resize(width);
	if (width > m_width) {
	    vtAttr space = line[0];
	    space.set_code(32);
	    space.set_mark(0);
	    for (int x = m_width; x < width; x++) {
		line[x] = space;
	    }
	}
    }
    for (int y = 0; y < m_backlog.count(); y++) {
	vtLine& line = m_backlog[y];
	line.resize(width);
	if (width > m_width) {
	    vtAttr space = line[0];
	    space.set_code(32);
	    space.set_mark(0);
	    for (int x = m_width; x < width; x++) {
		line[x] = space;
	    }
	}
    }
    if (height < m_height) {
	for (int y = 0; y < m_height - height; y++)
	    add_backlog(m_screen.takeFirst());
    }
    while (height > m_height && !m_backlog.isEmpty()) {
	m_screen.insert(0, m_backlog.takeLast());
	m_height++;
    }
    for (int y = m_height; y < height; y++) {
	m_screen += vtLine(width, m_def);
    }
    m_deccolm = width;
    m_width = width;
    m_height = height;
    m_top = 0;
    m_bottom = height;
    emit UpdateSize();
}

void vt220::term_set_columns(int width)
{
    if (width <= 0)
	width = m_deccolm;
    term_set_size(width, m_height);
    set_newx(m_cursor.newx);
}

void vt220::term_set_rows(int height)
{
    if (height <= 0)
	return;
    term_set_size(m_width, height);
    set_newx(m_cursor.newx);
}

void vt220::term_toggle_80_132()
{
    FUN("term_toggle_80_132");
    m_deccolm = 80 == m_deccolm ? 132 : 80;
    qDebug("%s: toggle to %d columns", _func, m_deccolm);
    term_set_columns();
}

void vt220::save()
{
    FUN("save");
    m_cursor_saved = m_cursor;
    m_att_saved = m_att;
}

void vt220::restore()
{
    FUN("restore");
    vt_CUP(m_cursor_saved.x, m_cursor_saved.y);
    m_att = m_att_saved;
    m_trans = m_gmaps[m_att.charset() ? m_att.gr() : m_att.gl()];
}

void vt220::putch(uchar ch)
{
    FUN("putch");
    CharacterMap map;
    QByteArray ba;
    uint tc = UC_INVALID;
    uchar ch2 = 0;

    for (;;) {
	quint32 ctrl = m_dspctrl ? CTRL_ALWAYS : CTRL_ACTION;

	if (m_utf_mode) {
	    if (ch & 0x80) {
		if (m_utf_more > 0) {
		    m_utf_code <<= 6;
		    if (0x80 == (ch & 0xc0)) {
			m_utf_code |= (ch & 0x3f);
			DBG_UNICODE("%s: UTF:%02x -> %08x (%d were left)", _func, ch, m_utf_code, m_utf_more);
			if (--m_utf_more == 0) {
			    tc = m_utf_code;
			    m_utf_code = 0;
			    if (tc < m_utf_code_min) {
				tc = UC_REPLACE;
			    }
			}
		    } else {
			DBG_UNICODE("%s: UTF:%02x -> %08x (%d invalid continuation)", _func, ch, m_utf_code, m_utf_more);
			m_utf_more = 0;
			tc = UC_REPLACE;
		    }
		} else if (0xc0 == (ch & 0xe0)) {
		    m_utf_more = 1;
		    m_utf_code = ch & 0x1f;
		    m_utf_code_min = 0x80;
		    DBG_UNICODE("%s: UTF:%02x -> %08x (%d more expected)", _func, ch, m_utf_code, m_utf_more);
		} else if (0xe0 == (ch & 0xf0)) {
		    m_utf_more = 2;
		    m_utf_code = ch & 0x0f;
		    m_utf_code_min = 0x800;
		    DBG_UNICODE("%s: UTF:%02x -> %08x (%d more expected)", _func, ch, m_utf_code, m_utf_more);
		} else if (0xf0 == (ch & 0xf8)) {
		    m_utf_more = 3;
		    m_utf_code = ch & 0x07;
		    m_utf_code_min = 0x10000;
		    DBG_UNICODE("%s: UTF:%02x -> %08x (%d more expected)", _func, ch, m_utf_code, m_utf_more);
		} else if (0xf8 == (ch & 0xfc)) {
		    m_utf_more = 4;
		    m_utf_code = ch & 0x03;
		    m_utf_code_min = 0x110000;
		    DBG_UNICODE("%s: UTF:%02x -> %08x (%d more expected)", _func, ch, m_utf_code, m_utf_more);
		} else if (0xfc == (ch & 0xfe)) {
		    m_utf_more = 5;
		    m_utf_code = ch & 0x01;
		    m_utf_code_min = 0x8000000;
		    DBG_UNICODE("%s: UTF:%02x -> %08x (%d more expected)", _func, ch, m_utf_code, m_utf_more);
		} else {
		    m_utf_more = 0;
		    m_utf_code = 0;
		    tc = UC_REPLACE;
		    DBG_UNICODE("%s: UTF:%02x -> %08x (%d invalid lead in)", _func, ch, m_utf_code, m_utf_more);
		}
	    } else {
		if (m_utf_more > 0) {
		    DBG_UNICODE("%s: UTF:%02x -> %08x (%d missing continuation)", _func, ch, m_utf_code, m_utf_more);
		    m_utf_more = 0;
		    m_utf_code = UC_INVALID;
		    ch2 = ch;
		    tc = UC_REPLACE;
		} else {
		    m_utf_code = ch;
		    // Default translation
		    tc = m_trans.value(ch, ch);
		    if (m_shift >= 0) {
			m_att.set_charset(uchar(m_shift));
			m_trans = m_gmaps[uchar(m_shift)];
			m_shift = -1;
		    }
#if DEBUG_SPAMLOG
		    DBG_UNICODE("%s: UTF:%02x -> %04x (CS0 character)", _func, ch, m_utf_char);
#endif
		}
	    }
	    if (m_utf_code != UC_INVALID && m_utf_more > 0)
		return;

	} else {

	    ch = uchar(m_togmeta ? ch | 0x80 : ch & 0x7f);
	    // Default translation
	    tc = m_trans.value(ch, ch);
	    if (m_shift >= 0) {
		m_att.set_charset(uchar(m_shift));
		m_trans = m_gmaps[uchar(m_shift)];
		m_shift = -1;
	    }

#if DEBUG_SPAMLOG
	    if (ch > 0x7f || m_togmeta) {
		DBG_UNICODE("%s: ch:%02x -> UTF:%04x", _func, ch, tc);
	    }
#endif
	}

	bool have_glyph = tc != UC_INVALID;
	have_glyph &= (ch >= 32 || (!m_utf_mode && 0 == ((ctrl >> ch) & 1)));
	have_glyph &= (ch != DEL || m_dspctrl);
	have_glyph &= (ch != CSI || m_utf_mode);

	if (have_glyph && !QChar::isPrint(tc)) {
	    DBG_UNICODE("%s: UTF:%08x -> %08x (not printable)", _func, tc, UC_REPLACE);
	    tc = UC_REPLACE;
	}

	if (!have_glyph || ESnormal != m_state)
	    break;

	QChar qc(tc);
	const int width = QChar::Mark_Enclosing == qc.category(tc) ? 2 : 1;
	if (QChar::Mark_NonSpacing == qc.category() ||
	    QChar::Mark_Enclosing == qc.category() ||
	    QChar::Mark_SpacingCombining == qc.category()) {
	    // marking character, e.g. dead character, enclosing, non-spacing combining
	    vtAttr chr(m_screen[m_cursor.y][m_cursor.x]);
	    chr.set_mark(tc);
	    chr.set_width(width);
	    outch(m_cursor.x, m_cursor.y, chr);
	} else {
	    // printing character
	    if (m_cursor.newx >= m_width) {
		if (m_decawm) {
		    vt_CR();
		    vt_LF();
		} else {
		    set_newx(m_width - 1);
		}
	    }
	    m_cursor.x = m_cursor.newx;

	    if (m_decim) {
		vt_ICH(1);
	    }

	    m_att.set_code(tc);
	    m_att.set_mark(0);
	    // m_att.set_width(1);
	    outch(m_cursor.x, m_cursor.y, m_att);
	}
	set_newx(m_cursor.x + width);
	if (!ch2)
	    return;

	ch = ch2;
	ch2 = 0;
    }

    if (ESnormal == m_state) {
	switch (ch) {
	case NUL:	// NUL
	    return;

	case BEL:	// BEL - Bell (Ctrl-G).
	    return;

	case BS:	// BS - Backspace (Ctrl-H).
	    vt_BS();
	    return;

	case HT:	// Horizontal Tab (Ctrl-I).
	    vt_TAB();
	    return;

	case LF:	// LF - Line Feed or New Line (Ctrl-J).
	    if (m_deccr) {
		// DEC auto carriage return
		vt_CR();
	    }
	    vt_LF();
	    return;

	case VT:	// VT - Cursor up (Ctrl-K).
	    if (m_deccr) {
		// DEC auto carriage return
		vt_CR();
	    }
	    vt_VT();
	    return;

	case FF:	// FF - Form Feed or New Page (Ctrl-L).
	    if (m_deccr) {
		// DEC auto carriage return
		vt_CR();
	    }
	    // vt_FF();
	    vt_LF();	// actually do only a LF
	    return;

	case CR:	// CR - Carriage Return (Ctrl-M).
	    vt_CR();
	    return;

	case SO:	// SO (LS1)
	    vt_LS1();
	    return;

	case SI:	// SI (LS0)
	    vt_LS0();
	    return;

	case CAN:	// CAN
	    m_state = ESnormal;
	    return;

	case SUB:	// SUB
	    m_state = ESnormal;
	    return;

	case ESC:	// ESC
	    m_state = ESesc;
	    return;

	case IND:	// IND
	    vt_IND();
	    return;

	case NEL:	// NEL
	    vt_NEL();
	    return;

	case HTS:	// HTS
	    vt_HTS();
	    return;

	case RI:	// RI
	    vt_RI();
	    return;

	case SS2:	// SS2
	    vt_SS2();
	    return;

	case SS3:	// SS3
	    vt_SS3();
	    return;

	case DCS:	// DCS
	    vt_DCS();
	    return;

	case SPA:	// SPA
	    vt_SPA();
	    return;

	case EPA:	// EPA
	    vt_EPA();
	    return;

	case SOS:	// SOS
	    vt_SOS();
	    return;

	case DECID:	// DECID
	    vt_DECID();
	    return;

	case CSI:	// CSI
	    m_state = EScsi;
	    break;

	case ST:	// ST
	    vt_ST();
	    return;

	case OSC:	// OSC
	    m_state = ESosc;
	    return;

	case PM:	// PM
	    vt_PM();
	    return;

	case APC:	// APC
	    vt_APC();
	    return;
	}
    }

    switch (m_state) {

    case ESesc:	// ESC state
	m_state = ESnormal;
	switch (ch) {
	case ETX:   // ESC ETX - Switch to VT100 Mode (ESC  Ctrl-C).
	    break;

	case ENQ:   // ESC ENQ - Return Terminal Status (ESC  Ctrl-E).
	    break;

	case FF:    // ESC FF  - PAGE (Clear Screen) (ESC  Ctrl-L).
	    vt_FF();
	    break;

	case SO:    // ESC SO  - Begin 4015 APL mode (ESC  Ctrl-N).  This is ignored by xterm.
	    break;

	case SI:    // ESC SI  - End 4015 APL mode (ESC  Ctrl-O).  This is ignored by xterm.
	    break;

	case ' ':   // SC7C1T, SC8C1T, ANSI conformance level
	    m_state = ESspc;
	    break;

	case '%':   // Unicode extension
	    m_state = ESperc;
	    break;

	case 'D':   // IND - Index (cursor down)
	    vt_IND();
	    break;

	case 'E':   // NEL - Next Line (cursor to start of row and down)
	    vt_NEL();
	    break;

	case 'H':   // HTS - Horizontal Tabulation Set
	    vt_HTS();
	    break;

	case 'M':   // RI  - Revers Index (cursor up)
	    vt_RI();
	    break;

	case 'N':   // SS2 - Single Shift to GS 2
	    vt_SS2();
	    break;

	case 'O':   // SS3 - Single Shift to GS 3
	    vt_SS3();
	    break;

	case 'P':   // DCS - Device Control String (DCS is 0x90).
	    vt_DCS();
	    break;

	case 'V':   // SPA - Start of Guarded Area (SPA is 0x96).
	    vt_SPA();
	    break;

	case 'W':   // EPA - End of Guarded Area (EPA is 0x97).
	    vt_EPA();
	    break;

	case 'X':   // SOS - Start of String (SOS is 0x98).
	    vt_SOS();
	    break;

	case 'Z':   // DECID - Return Terminal ID (DECID is 0x9a).  Obsolete form of CSI c  (DA).
	    vt_DECID();
	    break;

	case '[':   // CSI control sequence introducer
	    m_state = EScsi;
	    break;

	case '\\':  // ST  - String Terminator (ST  is 0x9c).
	    vt_ST();
	    break;

	case ']':   // OSC - Operating System Command (Linux special escape)
	    m_state = ESosc;
	    break;

	case '^':   // PM  - Privacy Message (PM  is 0x9e).
	    break;

	case '_':   // APC - Application Program Command (APC  is 0x9f).
	    break;

	case '7':   // save cursor
	    save();
	    break;

	case '8':   // restore cursor
	    restore();
	    break;

	case '=':   // Application Keypad (DECKPAM)
	    break;

	case '>':   // Normal keypad (DECKPNM)
	    break;

	case '(':   // GS0
	    m_state = ESsetG0_vt200;
	    break;

	case ')':   // GS1
	    m_state = ESsetG1_vt200;
	    break;

	case '*':   // GS2
	    m_state = ESsetG2_vt200;
	    break;

	case '+':   // GS3
	    m_state = ESsetG3_vt200;
	    break;

	case '#':   // Hash
	    m_state = EShash;
	    break;

	case 'c':   // Terminal reset
	    term_reset(m_terminal, m_width, m_height);
	    vt_ED(2);
	    break;

	case 'l':   // Memory lock (per HP terminals).
	    break;

	case 'm':   // Memory Unlock (per HP terminals).
	    break;

	case 'n':   // Invoke the G2 Character Set as GL (LS2).
	    vt_LS2();
	    break;

	case 'o':   // Invoke the G3 Character Set as GL (LS3).
	    vt_LS3();
	    break;

	case '~':   // Invoke the G1 Character Set as GR (LS1R).
	    vt_LS1R();
	    break;

	case '}':   // Invoke the G2 Character Set as GR (LS2R).
	    vt_LS2R();
	    break;

	case '|':   // Invoke the G3 Character Set as GR (LS3R).
	    vt_LS3R();
	    break;

	default:
	    qDebug("%s: ESC %c not handled (%d)", _func, ch, ch);
	}
	break;

    case ESspc:	// SC7C1T, SC8C1T, ANSI conformance level
	m_state = ESnormal;
	switch (ch) {
	case 'F':   // SC7C1T
	    m_s8c1t = false;
	    break;
	case 'G':   // SC8C1T
	    m_s8c1t = true;
	    break;
	case 'L':   // ANSI conformance level 1
	    m_ansi = 1;
	    break;
	case 'M':   // ANSI conformance level 2
	    m_ansi = 2;
	    break;
	case 'N':   // ANSI conformance level 3
	    m_ansi = 3;
	    break;
	default:
	    qDebug("%s: ESC SP %c not handled (%d)", _func, ch, ch);
	}
	break;

    case ESosc:
	m_state = ESnormal;
	switch (ch) {
	case 'P':	// set palette
	    m_state = ESpalette;
	    m_csi_args.clear();
	    break;
	case 'R':	// reset palette
	    vt_palette_reset();
	    break;
	default:
	    qDebug("%s: OSC %c not handled (%d)", _func, ch, ch);
	}
	break;

    case ESpalette:	// transfer palette entry
	m_csi_args.resize(m_csi_args.count() + 1);
	ba = QByteArray(1, ch);
	ba = ba.fromHex(ba);
	m_csi_args.append(ba.at(0));
	if (m_csi_args.count() == 7) {
	    const uchar i = uchar(m_csi_args[0]);
	    const uchar r = uchar((m_csi_args[1] << 4) | m_csi_args[2]);
	    const uchar g = uchar((m_csi_args[3] << 4) | m_csi_args[4]);
	    const uchar b = uchar((m_csi_args[5] << 4) | m_csi_args[6]);
	    m_pal[i] = qRgb(r, g, b);
	    qDebug("%s: set palette #%x R:%02x G:%02x B:%02x", _func,
		   i, r, g, b);
	    m_state = ESnormal;
	}
	break;

    case EScsi:	// CSI state
	m_state = ESgetargs;
	m_csi_args.fill(0x00, 1);
	if (ch == '[') {
	    m_state = ESfunckey;
	    return;
	}
	m_ques = ch == '?';
	if (m_ques)
	    break;
	// FALLTHROUGH

    case ESgetargs:
	switch (ch) {
	case ';':
	    m_csi_args.append(0);
	    break;

	case '0': case '1': case '2': case '3': case '4':
	case '5': case '6': case '7': case '8': case '9':
	    {
		int idx = m_csi_args.count() - 1;
		m_csi_args[idx] = m_csi_args[idx] * 10 + ch - '0';
	    }
	    break;

	default:
	    m_state = ESgotargs;
	}
	if (ESgotargs != m_state)
	    return;
	// FALLTHROUGH

    case ESgotargs:
	m_state = ESnormal;
	switch (ch) {
	case 'h':	// set state to on
	    vt_DECSET();
	    return;

	case 'l':	// set state to off
	    vt_RM();
	    return;

	case 'c':	// cursor type mask
	    if (!m_ques)
		break;

	    if (m_csi_args.count() > 2) {
		m_cursor_type = m_csi_args.value(0) + (m_csi_args.value(1) << 8) + (m_csi_args.value(2) << 16);
	    } else {
		m_cursor_type = 0;
	    }
	    break;

	case 'm':	// complement mode mask
	    if (!m_ques)
		break;

	    if (m_csi_args.count() > 1) {
		m_cc_mask = (m_csi_args.value(0) << 8) | m_csi_args.value(1);
	    } else {
		m_cc_mask = m_cc_save;
	    }
	    break;

	case 'n':	// CSI response requests
	    if (m_ques)
		break;

	    switch (m_csi_args.value(0)) {
	    case 5:
		qDebug("%s: <CSI>5n respond status", _func);
		emit term_response(QString("\033[0c").toLatin1());
		break;
	    case 6:
		qDebug("%s: <CSI>6n respond cursor", _func);
		emit term_response(QString("\033[%1;%2R").arg(m_cursor.y + 1).arg(m_cursor.x + 1).toLatin1());
		break;
	    default:
		qDebug("%s: <CSI>%dn respond cursor", _func, m_csi_args[0]);
		emit term_response(QString("\033[%1;%2H").arg(m_cursor.y + 1).arg(m_cursor.x + 1).toLatin1());
		break;
	    }
	    break;
	}

	if (m_ques) {
	    m_ques = false;
	    return;
	}

	switch (ch) {
	case '@':   // insert n characters
	    vt_ICH(m_csi_args.count() < 1 ? 1 : m_csi_args[0]);
	    break;

	case 'A':   // n times cursor up
	    vt_CUU(m_csi_args.count() < 1 ? 1 : m_csi_args[0]);
	    break;

	case 'B':   // n times cursor down
	case 'e':
	    vt_CUD(m_csi_args.count() < 1 ? 1 : m_csi_args[0]);
	    break;

	case 'C':   // n times cursor right
	case 'a':
	    m_cursor.x = m_cursor.newx;
	    vt_CUF(m_csi_args.count() < 1 ? 1 : m_csi_args[0]);
	    break;

	case 'D':   // n times cursor left
	    m_cursor.x = m_cursor.newx;
	    vt_CUB(m_csi_args.count() < 1 ? 1 : m_csi_args[0]);
	    break;

	case 'E':   // n rows down
	    vt_CNL(m_csi_args.count() < 1 ? 1 : m_csi_args[0]);
	    break;

	case 'F':   // n rows up
	    vt_CPL(m_csi_args.count() < 1 ? 1 : m_csi_args[0]);
	    break;

	case 'G':   // cursor character absolute (default row, 1)
	case '`':   // set cursor column (variant)
	    vt_CHA(m_csi_args.count() < 1 ? 1 : m_csi_args[0]);
	    break;

	case 'H':   // set cursor address
	case 'f':   // set cursor address (variant)
	    if (m_csi_args.count() < 1)
		m_csi_args.append(1);
	    if (m_csi_args.count() < 2)
		m_csi_args.append(1);
	    vt_CUA(m_csi_args[1] - 1, m_csi_args[0] - 1);
	    break;

	case 'I':   // cursor forward tabulation Ps tab stops (default 1)
	    vt_CHT(m_csi_args.count() < 1 ? 0 : m_csi_args[0]);
	    break;

	case 'J':   // clear screen
	    vt_ED(m_csi_args.count() < 1 ? 0 : m_csi_args[0]);
	    break;

	case 'K':   // clear line
	    vt_EL(m_csi_args.count() < 1 ? 0 : m_csi_args[0]);
	    break;

	case 'L':   // insert line
	    vt_IL(m_csi_args.count() < 1 ? 1 : m_csi_args[0]);
	    break;

	case 'M':   // delete line
	    vt_DL(m_csi_args.count() < 1 ? 1 : m_csi_args[0]);
	    break;

	case 'P':   // delete character
	    m_csi_args.resize(1);
	    if (0 == m_csi_args[0])
		m_csi_args[0] = 1;
	    vt_DCH(m_csi_args[0]);
	    break;

	case 'X':   // ECH - erase n characters
	    m_csi_args.resize(1);
	    if (0 == m_csi_args[0])
		m_csi_args[0] = 1;
	    vt_ECH(m_csi_args[0]);
	    break;

	case 'c':   // Send Device Attributes (Primary DA)
	    m_csi_args.resize(1);
	    qDebug("%s: respond ID (%d)", _func, m_csi_args[0]);
	    vt_DECID(m_csi_args[0]);
	    break;

	case 'd':   // set cursor row
	    if (m_csi_args.count() < 1)
		m_csi_args.append(1);
	    m_cursor.x = m_cursor.newx;
	    vt_CUA(m_cursor.x, m_csi_args[0] - 1);
	    break;

	case 'g':	// reset tabstop
	    m_csi_args.resize(1);
	    switch (m_csi_args[0]) {
	    case 0:
		qDebug("%s: clear tabstop (%d)", _func, m_cursor.newx);
		m_tabstop.clearBit(m_cursor.newx);
		break;
	    case 3:
		qDebug("%s: reset all tabstop marks", _func);
		m_tabstop.fill(false, 256);
		break;
	    default:
		qDebug("%s: invalid reset tabstop command", _func);
	    }
	    break;

	case 'm':	// CSI m
	    vt_SGR();
	    break;

	case 'q':	// DECLL (set leds)
	    break;

	case 'r':	// set region
	    if (m_csi_args.count() < 1)
		m_csi_args.append(1);
	    if (m_csi_args.count() < 1)
		m_csi_args.append(m_height);
	    if (m_csi_args[0] < m_csi_args[1] && m_csi_args[1] <= m_height) {
		m_top = m_csi_args[0] - 1;
		m_bottom = m_csi_args[1];
		vt_CUA(0, 0);
	    }
	    break;

	case 's':	// save cursor
	    save();
	    break;

	case 'u':	// restore cursor
	    restore();
	    break;

	case ']':	// Linux console setterm commands
	    vt_OSC();
	    break;
	}
	break;

    case ESstring:
	if (m_string.isEmpty()) {
	    m_string += QChar(ch);
	    break;
	}
	if (ch == '\\' && m_string.right(1) == QChar(ESC)) {
	    // ST
	    m_string.chop(1);
	    m_state = ESnormal;
	    break;
	}
	if (ch == ST) {
	    m_state = ESnormal;
	    break;
	}
	m_string += QChar(ch);
	break;

    case ESperc:
	m_state = ESnormal;
	switch (ch) {
	case '@':   // defined in ISO 2022
	    qDebug("%s: UTF-8 mode off", _func);
	    m_utf_mode = false;
	    break;
	case 'G':   // preliminary official escape code
	    qDebug("%s: UTF-8 mode on", _func);
	    m_utf_mode = true;
	    break;
	case '8':   // retained for compatibility
	    qDebug("%s: UTF-8 mode on", _func);
	    m_utf_mode = true;
	    break;
	}
	break;

    case ESsetG0_vt200:
	m_state = ESnormal;
	map = select_map_vt200(ch);
	if (MAP_VT500 == map) {
	    m_state = ESsetG0_vt500;
	    break;
	}
	m_gmaps[0] = m_charmaps[map];
	break;

    case ESsetG0_vt500:
	m_state = ESnormal;
	map = select_map_vt500(ch);
	m_gmaps[0] = m_charmaps[map];
	break;

    case ESsetG1_vt200:
	m_state = ESnormal;
	map = select_map_vt200(ch);
	if (MAP_VT500 == map) {
	    m_state = ESsetG1_vt500;
	    break;
	}
	m_gmaps[1] = m_charmaps[map];
	break;

    case ESsetG1_vt500:
	m_state = ESnormal;
	map = select_map_vt500(ch);
	m_gmaps[1] = m_charmaps[map];
	break;

    case ESsetG2_vt200:
	m_state = ESnormal;
	map = select_map_vt200(ch);
	if (MAP_VT500 == map) {
	    m_state = ESsetG2_vt500;
	    break;
	}
	m_gmaps[2] = m_charmaps[map];
	break;

    case ESsetG2_vt500:
	m_state = ESnormal;
	map = select_map_vt500(ch);
	m_gmaps[2] = m_charmaps[map];
	break;

    case ESsetG3_vt200:
	m_state = ESnormal;
	map = select_map_vt200(ch);
	if (MAP_VT500 == map) {
	    m_state = ESsetG3_vt500;
	    break;
	}
	m_gmaps[3] = m_charmaps[map];
	break;

    case ESsetG3_vt500:
	m_state = ESnormal;
	map = select_map_vt500(ch);
	m_gmaps[3] = m_charmaps[map];
	break;

    case EShash:
	m_state = ESnormal;
	switch (ch) {
	case '3':   // ESC # 3  DEC double-height line, top half (DECDHL), VT100.
	    m_screen[m_cursor.y].set_decdhl(false);
	    break;
	case '4':   // ESC # 4  DEC double-height line, bottom half (DECDHL), VT100.
	    m_screen[m_cursor.y].set_decdhl(true);
	    break;
	case '5':   // ESC # 5  DEC single-width line (DECSWL), VT100.
	    m_screen[m_cursor.y].set_decswl();
	    break;
	case '6':   // ESC # 6  DEC double-width line (DECDWL), VT100.
	    m_screen[m_cursor.y].set_decdwl();
	    break;
	case '8':   // ESC # 8	DEC screen alignment test
	    break;
	}
	break;

    case ESfunckey:
	m_state = ESnormal;
	break;
    }
}

int vt220::write(const QByteArray& data)
{
    FUN("write(QByteArray)");
    bool was_on = m_cursor.on;
    set_cursor(false);
    foreach(const char ch, data) {
	putch(uchar(ch));
    }
    set_cursor(was_on);
    return data.length();
}

int vt220::write(const char *buff, size_t len)
{
    FUN("write(const char*, size_t)");
    return write(QByteArray::fromRawData(buff, len));
}

int vt220::vprintf(const char* fmt, va_list ap)
{
    FUN("vprintf");
    QByteArray data = QString::vasprintf(fmt, ap).toUtf8();
    return write(data);
}

int vt220::printf(const char *fmt, ...)
{
    FUN("printf");
    va_list ap;
    va_start(ap, fmt);
    QByteArray data = QString::vasprintf(fmt, ap).toUtf8();
    va_end(ap);
    return write(data);
}

void vt220::display_text(const QString& filename)
{
    QFile file(filename);
    if (file.open(QIODevice::ReadOnly)) {
	while (!file.atEnd()) {
	    QByteArray line = file.readLine();
	    write(line);
	}
    }
}

void vt220::display_maps()
{
    static const QByteArray crlf("\r\n");
    static const QByteArray cyan_on_blue("\033[36;44m");
    static const QByteArray white_on_black("\033[37;40;m");
    static const QByteArray yellow("\033[33m");
    static const QByteArray inverse("\033[7m");
    static const QByteArray normal("\033[0m");
    for (int i = 0; i < NRCS_COUNT; i++) {
	QString heading = tr("  %1 %2: %3")
			  .arg(i < NRCS_USASCII ? tr("Charmap") : tr("NRCS"))
			  .arg(i)
			  .arg(m_charmap_name.value(static_cast<CharacterMap>(i)))
			  .leftJustified(34);
	write(crlf);
	write(cyan_on_blue);
	write(heading.toUtf8());
	write(white_on_black);
	write(crlf);

	write(inverse);
	write(QString("  0123456789ABCDEF0123456789ABCDEF").toLatin1());
	write(normal);
	write(crlf);

	cmapHash map = m_charmaps[i];
	for (int ch = 0; ch < 256; ch++) {
	    if (!map.contains(ch))
		continue;
	    uint uc = map.value(ch, ch);
	    if (0 == ch % 32) {
		QString row = QString("%1…").arg(ch/16, 1, 16).toUpper();
		write(inverse);
		write(row.toUtf8());
		write(normal);
	    }
	    if (uc == 0)
		uc = 0x2400;
	    if (i > NRCS_USASCII && uc != m_charmaps[NRCS_USASCII].value(ch, ch)) {
		write(yellow);
	    }
	    write(QString(QChar(uc)).toUtf8());
	    if (i > NRCS_USASCII && uc != m_charmaps[NRCS_USASCII].value(ch, ch)) {
		write(white_on_black);
	    }
	    if (31 == ch % 32) {
		write(crlf);
	    }
	}
	write(crlf);
    }
}


/**
 * @class vTerm
 * The vTerm class implements a (subset of) VT220 terminal
 * escape sequence handling and display, i.e. is a virtual VT220 terminal.
 * <pre>
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *                        XTerm Control Sequences
 *
 *
 *                                Edward Moy
 *                    University of California, Berkeley
 *
 *                                Revised by
 *
 *                              Stephen Gildea
 *                           X Consortium (1994)
 *
 *                              Thomas Dickey
 *                       XFree86 Project (1996-2006)
 *                     invisible-island.net (2006-2020)
 *                updated for XTerm Patch #354 (2020/04/30)
 *
 *
 *
 *
 * Definitions
 *
 * Many controls use parameters, shown in italics.  If a control uses a
 * single parameter, only one parameter name is listed.  Some parameters
 * (along with separating ;  characters) may be optional.  Other characters
 * in the control are required.
 *
 * C    A single (required) character.
 *
 * Ps   A single (usually optional) numeric parameter, composed of one or
 *      more digits.
 *
 * Pm   Any number of single numeric parameters, separated by ;  charac-
 *      ter(s).  Individual values for the parameters are listed with Ps .
 *
 * Pt   A text parameter composed of printable characters.
 *
 *
 * Control Bytes, Characters, and Sequences
 *
 * ECMA-48 (aka "ISO 6429") documents C1 (8-bit) and C0 (7-bit) codes.
 * Those are respectively codes 128 to 159 and 0 to 31.  ECMA-48 avoids
 * referring to these codes as characters, because that term is associated
 * with graphic characters.  Instead, it uses "bytes" and "codes", with
 * occasional lapses to "characters" where the meaning cannot be mistaken.
 *
 * Controls (including the escape code 27) are processed once:
 *
 * o   This means that a C1 control can be mistaken for badly-formed UTF-8
 *     when the terminal runs in UTF-8 mode because C1 controls are valid
 *     continuation bytes of a UTF-8 encoded (multibyte) value.
 *
 * o   It is not possible to use a C1 control obtained from decoding the
 *     UTF-8 text, because that would require reprocessing the data.  Con-
 *     sequently there is no ambiguity in the way this document uses the
 *     term "character" to refer to bytes in a control sequence.
 *
 * The order of processing is a necessary consequence of the way ECMA-48 is
 * designed:
 *
 * o   Each byte sent to the terminal can be unambiguously determined to
 *     fall into one of a few categories (C0, C1 and graphic characters).
 *
 * o   ECMA-48 is modal; once it starts processing a control sequence, the
 *     terminal continues until the sequence is complete, or some byte is
 *     found which is not allowed in the sequence.
 *
 * o   Intermediate, parameter and final bytes may use the same codes as
 *     graphic characters, but they are processed as part of a control
 *     sequence and are not actually graphic characters.
 *
 * o   Eight-bit controls can have intermediate, etc., bytes in the range
 *     160 to 255.  Those can be treated as their counterparts in the range
 *     32 to 127.
 *
 * o   Single-byte controls can be handled separately from multi-byte con-
 *     trol sequences because ECMA-48's rules are unambiguous.
 *
 *     As a special case, ECMA-48 (section 9) mentions that the control
 *     functions shift-in and shift-out are allowed to occur within a 7-bit
 *     multibyte control sequence because those cannot alter the meaning of
 *     the control sequence.
 *
 * o   Some controls (such as OSC ) introduce a string mode, which is ended
 *     on a ST  (string terminator).
 *
 *     ECMA-48 describes only correct behavior, telling what types of char-
 *     acters are expected at each stage of the control sequences.  It says
 *     that the action taken in error recovery is implementation-dependent.
 *     XTerm decodes control sequences using a state machine.  It handles
 *     errors in decoding i.e., unexpected characters, by resetting to the
 *     initial (ground) state.  That is different from the treatment of
 *     unimplemented (but correctly formatted) features.
 *
 *     If an application does not send the string terminator, that is also
 *     an error from the standpoint of a user.  To accommodate users of
 *     those applications, xterm has resource settings which allow work-
 *     arounds:
 *
 *     o   The Linux console's palette sequences do not use a string termi-
 *         nator.  The brokenLinuxOSC resource setting tells xterm to
 *         ignore those particular sequences.
 *
 *     o   The terminal should accept single-byte controls within the
 *         string.  But some applications omit a string terminator, like
 *         the Linux console.  The brokenStringTerm resource setting tells
 *         xterm to exit string mode if it decodes a common control charac-
 *         ter such as carriage return before the string terminator.
 *
 *
 * C1 (8-Bit) Control Characters
 *
 * The xterm program recognizes both 8-bit and 7-bit control characters.
 * It generates 7-bit controls (by default) or 8-bit if S8C1T is enabled.
 * The following pairs of 7-bit and 8-bit control characters are equiva-
 * lent:
 *
 * ESC D
 *      Index (IND  is 0x84).
 *
 * ESC E
 *      Next Line (NEL  is 0x85).
 *
 * ESC H
 *      Tab Set (HTS  is 0x88).
 *
 * ESC M
 *      Reverse Index (RI  is 0x8d).
 *
 * ESC N
 *      Single Shift Select of G2 Character Set (SS2  is 0x8e), VT220.
 *      This affects next character only.
 *
 * ESC O
 *      Single Shift Select of G3 Character Set (SS3  is 0x8f), VT220.
 *      This affects next character only.
 *
 * ESC P
 *      Device Control String (DCS  is 0x90).
 *
 * ESC V
 *      Start of Guarded Area (SPA  is 0x96).
 *
 * ESC W
 *      End of Guarded Area (EPA  is 0x97).
 *
 * ESC X
 *      Start of String (SOS  is 0x98).
 *
 * ESC Z
 *      Return Terminal ID (DECID is 0x9a).  Obsolete form of CSI c  (DA).
 *
 * ESC [
 *      Control Sequence Introducer (CSI  is 0x9b).
 *
 * ESC \
 *      String Terminator (ST  is 0x9c).
 *
 * ESC ]
 *      Operating System Command (OSC  is 0x9d).
 *
 * ESC ^
 *      Privacy Message (PM  is 0x9e).
 *
 * ESC _
 *      Application Program Command (APC  is 0x9f).
 *
 *
 * These control characters are used in the vtXXX emulation.
 *
 *
 * VT100 Mode
 *
 * In this document, "VT100" refers not only to VT100/VT102, but also to
 * the succession of upward-compatible terminals produced by DEC (Digital
 * Equipment Corporation) from the mid-1970s for about twenty years.  For
 * brevity, the document refers to the related models:
 *   "VT200" as VT220/VT240,
 *   "VT300" as VT320/VT340,
 *   "VT400" as VT420, and
 *   "VT500" as VT510/VT520/VT525.
 *
 * Most of these control sequences are standard VT102 control sequences,
 * but there is support for later DEC VT terminals (i.e., VT220, VT320,
 * VT420, VT510), as well as ECMA-48 and aixterm color controls.  The only
 * VT102 feature not supported is auto-repeat, since the only way X pro-
 * vides for this will affect all windows.
 *
 * There are additional control sequences to provide xterm-dependent func-
 * tions, such as the scrollbar or window size.  Where the function is
 * specified by DEC or ECMA-48, the code assigned to it is given in paren-
 * theses.
 *
 * The escape codes to designate and invoke character sets are specified by
 * ISO 2022 (see that document for a discussion of character sets).
 *
 * Many of the features are optional; xterm can be configured and built
 * without support for them.
 *
 *
 * Single-character functions
 *
 * BEL       Bell (BEL  is Ctrl-G).
 *
 * BS        Backspace (BS  is Ctrl-H).
 *
 * CR        Carriage Return (CR  is Ctrl-M).
 *
 * ENQ       Return Terminal Status (ENQ  is Ctrl-E).  Default response is
 *           an empty string, but may be overridden by a resource answer-
 *           backString.
 *
 * FF        Form Feed or New Page (NP ).  (FF  is Ctrl-L).  FF  is treated
 *           the same as LF .
 *
 * LF        Line Feed or New Line (NL).  (LF  is Ctrl-J).
 *
 * SI        Switch to Standard Character Set (Ctrl-O is Shift In or LS0).
 *           This invokes the G0 character set (the default) as GL.
 *           VT200 and up implement LS0.
 *
 * SO        Switch to Alternate Character Set (Ctrl-N is Shift Out or
 *           LS1).  This invokes the G1 character set as GL.
 *           VT200 and up implement LS1.
 *
 * SP        Space.
 *
 * TAB       Horizontal Tab (HTS  is Ctrl-I).
 *
 * VT        Vertical Tab (VT  is Ctrl-K).  This is treated the same as LF.
 *
 *
 * Controls beginning with ESC
 *
 * This excludes controls where ESC  is part of a 7-bit equivalent to 8-bit
 * C1 controls, ordered by the final character(s).
 *
 * ESC SP F  7-bit controls (S7C1T), VT220.  This tells the terminal to
 *           send C1 control characters as 7-bit sequences, e.g., its
 *           responses to queries.  DEC VT200 and up always accept 8-bit
 *           control sequences except when configured for VT100 mode.
 *
 * ESC SP G  8-bit controls (S8C1T), VT220.  This tells the terminal to
 *           send C1 control characters as 8-bit sequences, e.g., its
 *           responses to queries.  DEC VT200 and up always accept 8-bit
 *           control sequences except when configured for VT100 mode.
 *
 * ESC SP L  Set ANSI conformance level 1, ECMA-43.
 *
 * ESC SP M  Set ANSI conformance level 2, ECMA-43.
 *
 * ESC SP N  Set ANSI conformance level 3, ECMA-43.
 *
 * ESC # 3   DEC double-height line, top half (DECDHL), VT100.
 *
 * ESC # 4   DEC double-height line, bottom half (DECDHL), VT100.
 *
 * ESC # 5   DEC single-width line (DECSWL), VT100.
 *
 * ESC # 6   DEC double-width line (DECDWL), VT100.
 *
 * ESC # 8   DEC Screen Alignment Test (DECALN), VT100.
 *
 * ESC % @   Select default character set.  That is ISO 8859-1 (ISO 2022).
 *
 * ESC % G   Select UTF-8 character set, ISO 2022.
 *
 * ESC ( C   Designate G0 Character Set, VT100, ISO 2022.
 *           Final character C for designating 94-character sets.  In this
 *           list,
 *           o   0 , A  and B  were introduced in the VT100,
 *           o   most were introduced in the VT200 series,
 *           o   a few were introduced in the VT300 series, and
 *           o   a few more were introduced in the VT500 series.
 *           The VT220 character sets, together with a few others (such as
 *           Portuguese) are activated by the National Replacement Charac-
 *           ter Set (NRCS) controls.  The term "replacement" says that the
 *           character set is formed by replacing some of the characters in
 *           a set (termed the Multinational Character Set) with more use-
 *           ful ones for a given language.  The ASCII and DEC Supplemental
 *           character sets make up the two halves of the Multinational
 *           Character set, initially mapped to GL and GR.
 *           The valid final characters C for this control are:
 *             C = A  -> United Kingdom (UK), VT100.
 *             C = B  -> United States (USASCII), VT100.
 *             C = C  or 5  -> Finnish, VT200.
 *             C = H  or 7  -> Swedish, VT200.
 *             C = K  -> German, VT200.
 *             C = Q  or 9  -> French Canadian, VT200.
 *             C = R  or f  -> French, VT200.
 *             C = Y  -> Italian, VT200.
 *             C = Z  -> Spanish, VT200.
 *             C = 4  -> Dutch, VT200.
 *             C = " >  -> Greek, VT500.
 *             C = % 2  -> Turkish, VT500.
 *             C = % 6  -> Portuguese, VT300.
 *             C = % =  -> Hebrew, VT500.
 *             C = =  -> Swiss, VT200.
 *             C = ` , E  or 6  -> Norwegian/Danish, VT200.
 *           The final character A  is a special case, since the same final
 *           character is used by the VT300-control for the 96-character
 *           British Latin-1.
 *           There are a few other 94-character sets:
 *             C = 0  -> DEC Special Character and Line Drawing Set, VT100.
 *             C = <  -> DEC Supplemental, VT200.
 *             C = >  -> DEC Technical, VT300.
 *           These are documented as NRCS:
 *             C = " 4  -> DEC Hebrew, VT500.
 *             C = " ?  -> DEC Greek, VT500.
 *             C = % 0  -> DEC Turkish, VT500.
 *             C = % 5  -> DEC Supplemental Graphics, VT300.
 *             C = & 4  -> DEC Cyrillic, VT500.
 *           The VT520 reference manual lists a few more, but no documenta-
 *           tion has been found for the mappings:
 *             C = % 3  -> SCS NRCS, VT500.
 *             C = & 5  -> DEC Russian, VT500.
 *
 * ESC ) C   Designate G1 Character Set, ISO 2022, VT100.
 *           The same character sets apply as for ESC ( C.
 *
 * ESC * C   Designate G2 Character Set, ISO 2022, VT220.
 *           The same character sets apply as for ESC ( C.
 *
 * ESC + C   Designate G3 Character Set, ISO 2022, VT220.
 *           The same character sets apply as for ESC ( C.
 *
 * ESC - C   Designate G1 Character Set, VT300.
 *           These controls apply only to 96-character sets.  Unlike the
 *           94-character sets, these can have different values than ASCII
 *           space and DEL for the mapping of 0x20 and 0x7f.  The valid
 *           final characters C for this control are:
 *             C = A  -> ISO Latin-1 Supplemental, VT300.
 *             C = F  -> ISO Greek Supplemental, VT500.
 *             C = H  -> ISO Hebrew Supplemental, VT500.
 *             C = L  -> ISO Latin-Cyrillic, VT500.
 *             C = M  -> ISO Latin-5 Supplemental, VT500.
 *
 * ESC . C   Designate G2 Character Set, VT300.
 *           The same character sets apply as for ESC - C.
 *
 * ESC / C   Designate G3 Character Set, VT300.
 *           The same character sets apply as for ESC - C.
 *
 * ESC 6     Back Index (DECBI), VT420 and up.
 *
 * ESC 7     Save Cursor (DECSC), VT100.
 *
 * ESC 8     Restore Cursor (DECRC), VT100.
 *
 * ESC 9     Forward Index (DECFI), VT420 and up.
 *
 * ESC =     Application Keypad (DECKPAM).
 *
 * ESC >     Normal Keypad (DECKPNM), VT100.
 *
 * ESC F     Cursor to lower left corner of screen.  This is enabled by the
 *           hpLowerleftBugCompat resource.
 *
 * ESC c     Full Reset (RIS), VT100.
 *
 * ESC l     Memory Lock (per HP terminals).  Locks memory above the cur-
 *           sor.
 *
 * ESC m     Memory Unlock (per HP terminals).
 *
 * ESC n     Invoke the G2 Character Set as GL (LS2) as GL.
 *
 * ESC o     Invoke the G3 Character Set as GL (LS3) as GL.
 *
 * ESC |     Invoke the G3 Character Set as GR (LS3R).
 *
 * ESC }     Invoke the G2 Character Set as GR (LS2R).
 *
 * ESC ~     Invoke the G1 Character Set as GR (LS1R), VT100.
 *
 *
 * Application Program-Command functions
 *
 * APC Pt ST None.  xterm implements no APC  functions; Pt is ignored.  Pt
 *           need not be printable characters.
 *
 *
 * Device-Control functions
 *
 * DCS Ps ; Ps | Pt ST
 *           User-Defined Keys (DECUDK), VT220 and up.
 *
 *           The first parameter:
 *             Ps = 0  -> Clear all UDK definitions before starting
 *           (default).
 *             Ps = 1  -> Erase Below (default).
 *
 *           The second parameter:
 *             Ps = 0  <- Lock the keys (default).
 *             Ps = 1  <- Do not lock.
 *
 *           The third parameter is a ';'-separated list of strings denot-
 *           ing the key-code separated by a '/' from the hex-encoded key
 *           value.  The key codes correspond to the DEC function-key codes
 *           (e.g., F6=17).
 *
 * DCS $ q Pt ST
 *           Request Status String (DECRQSS), VT420 and up.
 *           The string following the "q" is one of the following:
 *             m       -> SGR
 *             " p     -> DECSCL
 *             SP q    -> DECSCUSR
 *             " q     -> DECSCA
 *             r       -> DECSTBM
 *             s       -> DECSLRM
 *             t       -> DECSLPP
 *             $ |     -> DECSCPP
 *             * |     -> DECSNLS
 *           xterm responds with DCS 1 $ r Pt ST for valid requests,
 *           replacing the Pt with the corresponding CSI string, or DCS 0 $
 *           r Pt ST for invalid requests.
 *
 * DCS Ps $ t Pt ST
 *           Restore presentation status (DECRSPS), VT320 and up.  The con-
 *           trol can be converted from a response from DECCIR or DECTABSR
 *           by changing the first "u" to a "t"
 *             Ps = 1  -> DECCIR
 *             Ps = 2  -> DECTABSR
 *
 * DCS + Q Pt ST
 *           Request resource values (XTGETXRES), xterm.  The string fol-
 *           lowing the "Q" is a list of names encoded in hexadecimal (2
 *           digits per character) separated by ; which correspond to xterm
 *           resource names.  Only boolean, numeric and string resources
 *           are supported by this query.
 *
 *           xterm responds with
 *           DCS 1 + R Pt ST for valid requests, adding to Pt an = , and
 *           the value of the corresponding resource that xterm is using,
 *           or
 *           DCS 0 + R Pt ST for invalid requests.
 *           The strings are encoded in hexadecimal (2 digits per charac-
 *           ter).
 *
 *
 * DCS + p Pt ST
 *           Set Termcap/Terminfo Data (XTSETTCAP), xterm.  The string fol-
 *           lowing the "p" is a name to use for retrieving data from the
 *           terminal database.  The data will be used for the "tcap" key-
 *           board configuration's function- and special-keys, as well as
 *           by the Request Termcap/Terminfo String control.
 *
 *
 * DCS + q Pt ST
 *           Request Termcap/Terminfo String (XTGETTCAP), xterm.  The
 *           string following the "q" is a list of names encoded in hexa-
 *           decimal (2 digits per character) separated by ; which corre-
 *           spond to termcap or terminfo key names.
 *           A few special features are also recognized, which are not key
 *           names:
 *
 *           o   Co for termcap colors (or colors for terminfo colors), and
 *
 *           o   TN for termcap name (or name for terminfo name).
 *
 *           o   RGB for the ncurses direct-color extension.
 *               Only a terminfo name is provided, since termcap applica-
 *               tions cannot use this information.
 *
 *           xterm responds with
 *           DCS 1 + r Pt ST for valid requests, adding to Pt an = , and
 *           the value of the corresponding string that xterm would send,
 *           or
 *           DCS 0 + r Pt ST for invalid requests.
 *           The strings are encoded in hexadecimal (2 digits per charac-
 *           ter).
 *
 *
 * Functions using CSI , ordered by the final character(s)
 *
 * CSI Ps @  Insert Ps (Blank) Character(s) (default = 1) (ICH).
 *
 * CSI Ps SP @
 *           Shift left Ps columns(s) (default = 1) (SL), ECMA-48.
 *
 * CSI Ps A  Cursor Up Ps Times (default = 1) (CUU).
 *
 * CSI Ps SP A
 *           Shift right Ps columns(s) (default = 1) (SR), ECMA-48.
 *
 * CSI Ps B  Cursor Down Ps Times (default = 1) (CUD).
 *
 * CSI Ps C  Cursor Forward Ps Times (default = 1) (CUF).
 *
 * CSI Ps D  Cursor Backward Ps Times (default = 1) (CUB).
 *
 * CSI Ps E  Cursor Next Line Ps Times (default = 1) (CNL).
 *
 * CSI Ps F  Cursor Preceding Line Ps Times (default = 1) (CPL).
 *
 * CSI Ps G  Cursor Character Absolute  [column] (default = [row,1]) (CHA).
 *
 * CSI Ps ; Ps H
 *           Cursor Position [row;column] (default = [1,1]) (CUP).
 *
 * CSI Ps I  Cursor Forward Tabulation Ps tab stops (default = 1) (CHT).
 *
 * CSI Ps J  Erase in Display (ED), VT100.
 *             Ps = 0  -> Erase Below (default).
 *             Ps = 1  -> Erase Above.
 *             Ps = 2  -> Erase All.
 *             Ps = 3  -> Erase Saved Lines, xterm.
 *
 * CSI ? Ps J
 *           Erase in Display (DECSED), VT220.
 *             Ps = 0  -> Selective Erase Below (default).
 *             Ps = 1  -> Selective Erase Above.
 *             Ps = 2  -> Selective Erase All.
 *             Ps = 3  -> Selective Erase Saved Lines, xterm.
 *
 * CSI Ps K  Erase in Line (EL), VT100.
 *             Ps = 0  -> Erase to Right (default).
 *             Ps = 1  -> Erase to Left.
 *             Ps = 2  -> Erase All.
 *
 * CSI ? Ps K
 *           Erase in Line (DECSEL), VT220.
 *             Ps = 0  -> Selective Erase to Right (default).
 *             Ps = 1  -> Selective Erase to Left.
 *             Ps = 2  -> Selective Erase All.
 *
 * CSI Ps L  Insert Ps Line(s) (default = 1) (IL).
 *
 * CSI Ps M  Delete Ps Line(s) (default = 1) (DL).
 *
 * CSI Ps P  Delete Ps Character(s) (default = 1) (DCH).
 *
 * CSI Ps S  Scroll up Ps lines (default = 1) (SU), VT420, ECMA-48.
 *
 * CSI ? Pi ; Pa ; Pv S
 *           Set or request graphics attribute, xterm.  If configured to
 *           support either Sixel Graphics or ReGIS Graphics, xterm accepts
 *           a three-parameter control sequence, where Pi, Pa and Pv are
 *           the item, action and value:
 *
 *             Pi = 1  -> item is number of color registers.
 *             Pi = 2  -> item is Sixel graphics geometry (in pixels).
 *             Pi = 3  -> item is ReGIS graphics geometry (in pixels).
 *
 *             Pa = 1  -> read attribute.
 *             Pa = 2  -> reset to default.
 *             Pa = 3  -> set to value in Pv.
 *             Pa = 4  -> read the maximum allowed value.
 *
 *             Pv is ignored by xterm except when setting (Pa == 3 ).
 *             Pv = n <- A single integer is used for color registers.
 *             Pv = width ; height <- Two integers for graphics geometry.
 *
 *           xterm replies with a control sequence of the same form:
 *
 *                CSI ? Pi ; Ps ; Pv S
 *
 *           where Ps is the status:
 *             Ps = 0  <- success.
 *             Ps = 1  <- error in Pi.
 *             Ps = 2  <- error in Pa.
 *             Ps = 3  <- failure.
 *
 *           On success, Pv represents the value read or set.
 *
 *           Notes:
 *           o   The current implementation allows reading the graphics
 *               sizes, but disallows modifying those sizes because that is
 *               done once, using resource-values.
 *           o   Graphics geometry is not necessarily the same as "window
 *               size" (see the dtterm window manipulation extensions).
 *               For example, xterm limits the maximum graphics geometry at
 *               compile time (1000x1000 as of version 328) although the
 *               window size can be larger.
 *           o   While resizing a window will always change the current
 *               graphics geometry, the reverse is not true.  Setting
 *               graphics geometry does not affect the window size.
 *
 * CSI Ps T  Scroll down Ps lines (default = 1) (SD), VT420.
 *
 * CSI Ps ; Ps ; Ps ; Ps ; Ps T
 *           Initiate highlight mouse tracking.  Parameters are
 *           [func;startx;starty;firstrow;lastrow].  See the section Mouse
 *           Tracking.
 *
 * CSI > Pm T
 *           Reset title mode features to default value, xterm.  Normally,
 *           "reset" disables the feature.  It is possible to disable the
 *           ability to reset features by compiling a different default for
 *           the title modes into xterm.
 *
 *             Ps = 0  -> Do not set window/icon labels using hexadecimal.
 *             Ps = 1  -> Do not query window/icon labels using hexadeci-
 *           mal.
 *             Ps = 2  -> Do not set window/icon labels using UTF-8.
 *             Ps = 3  -> Do not query window/icon labels using UTF-8.
 *
 *           (See discussion of Title Modes).
 *
 * CSI Ps X  Erase Ps Character(s) (default = 1) (ECH).
 *
 * CSI Ps Z  Cursor Backward Tabulation Ps tab stops (default = 1) (CBT).
 *
 * CSI Ps ^  Scroll down Ps lines (default = 1) (SD), ECMA-48.
 *           This was a publication error in the original ECMA-48 5th edi-
 *           tion (1991) corrected in 2003.
 *
 * CSI Pm `  Character Position Absolute  [column] (default = [row,1])
 *           (HPA).
 *
 * CSI Pm a  Character Position Relative  [columns] (default = [row,col+1])
 *           (HPR).
 *
 * CSI Ps b  Repeat the preceding graphic character Ps times (REP).
 *
 * CSI Ps c  Send Device Attributes (Primary DA).
 *             Ps = 0  or omitted -> request attributes from terminal.  The
 *           response depends on the decTerminalID resource setting.
 *             -> CSI ? 1 ; 2 c  ("VT100 with Advanced Video Option")
 *             -> CSI ? 1 ; 0 c  ("VT101 with No Options")
 *             -> CSI ? 6 c  ("VT102")
 *             -> CSI ? 1 2 ; Psc  ("VT125")
 *             -> CSI ? 6 2 ; Psc  ("VT220")
 *             -> CSI ? 6 3 ; Psc  ("VT320")
 *             -> CSI ? 6 4 ; Psc  ("VT420")
 *
 *           The VT100-style response parameters do not mean anything by
 *           themselves.  VT220 (and higher) parameters do, telling the
 *           host what features the terminal supports:
 *             Ps = 1  -> 132-columns.
 *             Ps = 2  -> Printer.
 *             Ps = 3  -> ReGIS graphics.
 *             Ps = 4  -> Sixel graphics.
 *             Ps = 6  -> Selective erase.
 *             Ps = 8  -> User-defined keys.
 *             Ps = 9  -> National Replacement Character sets.
 *             Ps = 1 5  -> Technical characters.
 *             Ps = 1 6  -> Locator port.
 *             Ps = 1 7  -> Terminal state interrogation.
 *             Ps = 1 8  -> User windows.
 *             Ps = 2 1  -> Horizontal scrolling.
 *             Ps = 2 2  -> ANSI color, e.g., VT525.
 *             Ps = 2 8  -> Rectangular editing.
 *             Ps = 2 9  -> ANSI text locator (i.e., DEC Locator mode).
 *
 *           XTerm supports part of the User windows feature, providing a
 *           single page (which corresponds to its visible window).  Rather
 *           than resizing the font to change the number of lines/columns
 *           in a fixed-size display, xterm uses the window extension con-
 *           trols (DECSNLS, DECSCPP, DECSLPP) to adjust its visible win-
 *           dow's size.  The "cursor coupling" controls (DECHCCM, DECPCCM,
 *           DECVCCM) are ignored.
 *
 * CSI = Ps c
 *           Send Device Attributes (Tertiary DA).
 *             Ps = 0  -> report Terminal Unit ID (default), VT400.  XTerm
 *           uses zeros for the site code and serial number in its DECRPTUI
 *           response.
 *
 * CSI > Ps c
 *           Send Device Attributes (Secondary DA).
 *             Ps = 0  or omitted -> request the terminal's identification
 *           code.  The response depends on the decTerminalID resource set-
 *           ting.  It should apply only to VT220 and up, but xterm extends
 *           this to VT100.
 *             -> CSI  > Pp ; Pv ; Pc c
 *           where Pp denotes the terminal type
 *             Pp = 0  -> "VT100".
 *             Pp = 1  -> "VT220".
 *             Pp = 2  -> "VT240" or "VT241".
 *             Pp = 1 8  -> "VT330".
 *             Pp = 1 9  -> "VT340".
 *             Pp = 2 4  -> "VT320".
 *             Pp = 3 2  -> "VT382".
 *             Pp = 4 1  -> "VT420".
 *             Pp = 6 1  -> "VT510".
 *             Pp = 6 4  -> "VT520".
 *             Pp = 6 5  -> "VT525".
 *
 *           and Pv is the firmware version (for xterm, this was originally
 *           the XFree86 patch number, starting with 95).  In a DEC termi-
 *           nal, Pc indicates the ROM cartridge registration number and is
 *           always zero.
 *
 * CSI Pm d  Line Position Absolute  [row] (default = [1,column]) (VPA).
 *
 * CSI Pm e  Line Position Relative  [rows] (default = [row+1,column])
 *           (VPR).
 *
 * CSI Ps ; Ps f
 *           Horizontal and Vertical Position [row;column] (default =
 *           [1,1]) (HVP).
 *
 * CSI Ps g  Tab Clear (TBC).
 *             Ps = 0  -> Clear Current Column (default).
 *             Ps = 3  -> Clear All.
 *
 * CSI Pm h  Set Mode (SM).
 *             Ps = 2  -> Keyboard Action Mode (AM).
 *             Ps = 4  -> Insert Mode (IRM).
 *             Ps = 1 2  -> Send/receive (SRM).
 *             Ps = 2 0  -> Automatic Newline (LNM).
 *
 * CSI ? Pm h
 *           DEC Private Mode Set (DECSET).
 *             Ps = 1  -> Application Cursor Keys (DECCKM), VT100.
 *             Ps = 2  -> Designate USASCII for character sets G0-G3
 *           (DECANM), VT100, and set VT100 mode.
 *             Ps = 3  -> 132 Column Mode (DECCOLM), VT100.
 *             Ps = 4  -> Smooth (Slow) Scroll (DECSCLM), VT100.
 *             Ps = 5  -> Reverse Video (DECSCNM), VT100.
 *             Ps = 6  -> Origin Mode (DECOM), VT100.
 *             Ps = 7  -> Auto-wrap Mode (DECAWM), VT100.
 *             Ps = 8  -> Auto-repeat Keys (DECARM), VT100.
 *             Ps = 9  -> Send Mouse X & Y on button press.  See the sec-
 *           tion Mouse Tracking.  This is the X10 xterm mouse protocol.
 *             Ps = 1 0  -> Show toolbar (rxvt).
 *             Ps = 1 2  -> Start Blinking Cursor (AT&T 610).
 *             Ps = 1 3  -> Start Blinking Cursor (set only via resource or
 *           menu).
 *             Ps = 1 4  -> Enable XOR of Blinking Cursor control sequence
 *           and menu.
 *             Ps = 1 8  -> Print form feed (DECPFF), VT220.
 *             Ps = 1 9  -> Set print extent to full screen (DECPEX),
 *           VT220.
 *             Ps = 2 5  -> Show Cursor (DECTCEM), VT220.
 *             Ps = 3 0  -> Show scrollbar (rxvt).
 *             Ps = 3 5  -> Enable font-shifting functions (rxvt).
 *             Ps = 3 8  -> Enter Tektronix Mode (DECTEK), VT240, xterm.
 *             Ps = 4 0  -> Allow 80 -> 132 Mode, xterm.
 *             Ps = 4 1  -> more(1) fix (see curses resource).
 *             Ps = 4 2  -> Enable National Replacement Character sets
 *           (DECNRCM), VT220.
 *             Ps = 4 4  -> Turn On Margin Bell, xterm.
 *             Ps = 4 5  -> Reverse-wraparound Mode, xterm.
 *             Ps = 4 6  -> Start Logging, xterm.  This is normally dis-
 *           abled by a compile-time option.
 *             Ps = 4 7  -> Use Alternate Screen Buffer, xterm.  This may
 *           be disabled by the titeInhibit resource.
 *             Ps = 6 6  -> Application keypad (DECNKM), VT320.
 *             Ps = 6 7  -> Backarrow key sends backspace (DECBKM), VT340,
 *           VT420.  This sets the backarrowKey resource to "true".
 *             Ps = 6 9  -> Enable left and right margin mode (DECLRMM),
 *           VT420 and up.
 *             Ps = 8 0  -> Enable Sixel Scrolling (DECSDM).
 *             Ps = 9 5  -> Do not clear screen when DECCOLM is set/reset
 *           (DECNCSM), VT510 and up.
 *             Ps = 1 0 0 0  -> Send Mouse X & Y on button press and
 *           release.  See the section Mouse Tracking.  This is the X11
 *           xterm mouse protocol.
 *             Ps = 1 0 0 1  -> Use Hilite Mouse Tracking, xterm.
 *             Ps = 1 0 0 2  -> Use Cell Motion Mouse Tracking, xterm.  See
 *           the section Button-event tracking.
 *             Ps = 1 0 0 3  -> Use All Motion Mouse Tracking, xterm.  See
 *           the section Any-event tracking.
 *             Ps = 1 0 0 4  -> Send FocusIn/FocusOut events, xterm.
 *             Ps = 1 0 0 5  -> Enable UTF-8 Mouse Mode, xterm.
 *             Ps = 1 0 0 6  -> Enable SGR Mouse Mode, xterm.
 *             Ps = 1 0 0 7  -> Enable Alternate Scroll Mode, xterm.  This
 *           corresponds to the alternateScroll resource.
 *             Ps = 1 0 1 0  -> Scroll to bottom on tty output (rxvt).
 *           This sets the scrollTtyOutput resource to "true".
 *             Ps = 1 0 1 1  -> Scroll to bottom on key press (rxvt).  This
 *           sets the scrollKey resource to "true".
 *             Ps = 1 0 1 5  -> Enable urxvt Mouse Mode.
 *             Ps = 1 0 3 4  -> Interpret "meta" key, xterm.  This sets the
 *           eighth bit of keyboard input (and enables the eightBitInput
 *           resource).
 *             Ps = 1 0 3 5  -> Enable special modifiers for Alt and Num-
 *           Lock keys, xterm.  This enables the numLock resource.
 *             Ps = 1 0 3 6  -> Send ESC   when Meta modifies a key, xterm.
 *           This enables the metaSendsEscape resource.
 *             Ps = 1 0 3 7  -> Send DEL from the editing-keypad Delete
 *           key, xterm.
 *             Ps = 1 0 3 9  -> Send ESC  when Alt modifies a key, xterm.
 *           This enables the altSendsEscape resource, xterm.
 *             Ps = 1 0 4 0  -> Keep selection even if not highlighted,
 *           xterm.  This enables the keepSelection resource.
 *             Ps = 1 0 4 1  -> Use the CLIPBOARD selection, xterm.  This
 *           enables the selectToClipboard resource.
 *             Ps = 1 0 4 2  -> Enable Urgency window manager hint when
 *           Control-G is received, xterm.  This enables the bellIsUrgent
 *           resource.
 *             Ps = 1 0 4 3  -> Enable raising of the window when Control-G
 *           is received, xterm.  This enables the popOnBell resource.
 *             Ps = 1 0 4 4  -> Reuse the most recent data copied to CLIP-
 *           BOARD, xterm.  This enables the keepClipboard resource.
 *             Ps = 1 0 4 6  -> Enable switching to/from Alternate Screen
 *           Buffer, xterm.  This works for terminfo-based systems, updat-
 *           ing the titeInhibit resource.
 *             Ps = 1 0 4 7  -> Use Alternate Screen Buffer, xterm.  This
 *           may be disabled by the titeInhibit resource.
 *             Ps = 1 0 4 8  -> Save cursor as in DECSC, xterm.  This may
 *           be disabled by the titeInhibit resource.
 *             Ps = 1 0 4 9  -> Save cursor as in DECSC, xterm.  After sav-
 *           ing the cursor, switch to the Alternate Screen Buffer, clear-
 *           ing it first.  This may be disabled by the titeInhibit
 *           resource.  This control combines the effects of the 1 0 4 7
 *           and 1 0 4 8  modes.  Use this with terminfo-based applications
 *           rather than the 4 7  mode.
 *             Ps = 1 0 5 0  -> Set terminfo/termcap function-key mode,
 *           xterm.
 *             Ps = 1 0 5 1  -> Set Sun function-key mode, xterm.
 *             Ps = 1 0 5 2  -> Set HP function-key mode, xterm.
 *             Ps = 1 0 5 3  -> Set SCO function-key mode, xterm.
 *             Ps = 1 0 6 0  -> Set legacy keyboard emulation, i.e, X11R6,
 *           xterm.
 *             Ps = 1 0 6 1  -> Set VT220 keyboard emulation, xterm.
 *             Ps = 2 0 0 4  -> Set bracketed paste mode, xterm.
 *
 * CSI Pm i  Media Copy (MC).
 *             Ps = 0  -> Print screen (default).
 *             Ps = 4  -> Turn off printer controller mode.
 *             Ps = 5  -> Turn on printer controller mode.
 *             Ps = 1 0  -> HTML screen dump, xterm.
 *             Ps = 1 1  -> SVG screen dump, xterm.
 *
 * CSI ? Pm i
 *           Media Copy (MC), DEC-specific.
 *             Ps = 1  -> Print line containing cursor.
 *             Ps = 4  -> Turn off autoprint mode.
 *             Ps = 5  -> Turn on autoprint mode.
 *             Ps = 1 0  -> Print composed display, ignores DECPEX.
 *             Ps = 1 1  -> Print all pages.
 *
 * CSI Pm l  Reset Mode (RM).
 *             Ps = 2  -> Keyboard Action Mode (AM).
 *             Ps = 4  -> Replace Mode (IRM).
 *             Ps = 1 2  -> Send/receive (SRM).
 *             Ps = 2 0  -> Normal Linefeed (LNM).
 *
 * CSI ? Pm l
 *           DEC Private Mode Reset (DECRST).
 *             Ps = 1  -> Normal Cursor Keys (PM), VT100.
 *             Ps = 2  -> Designate VT52 mode (DECANM), VT100.
 *             Ps = 3  -> 80 Column Mode (DECCOLM), VT100.
 *             Ps = 4  -> Jump (Fast) Scroll (DECSCLM), VT100.
 *             Ps = 5  -> Normal Video (DECSCNM), VT100.
 *             Ps = 6  -> Normal Cursor Mode (DECOM), VT100.
 *             Ps = 7  -> No Auto-wrap Mode (DECAWM), VT100.
 *             Ps = 8  -> No Auto-repeat Keys (DECARM), VT100.
 *             Ps = 9  -> Don't send Mouse X & Y on button press, xterm.
 *             Ps = 1 0  -> Hide toolbar (rxvt).
 *             Ps = 1 2  -> Stop Blinking Cursor (AT&T 610).
 *             Ps = 1 3  -> Disable Blinking Cursor (reset only via
 *           resource or menu).
 *             Ps = 1 4  -> Disable XOR of Blinking Cursor control sequence
 *           and menu.
 *             Ps = 1 8  -> Don't print form feed (DECPFF).
 *             Ps = 1 9  -> Limit print to scrolling region (DECPEX).
 *             Ps = 2 5  -> Hide Cursor (DECTCEM), VT220.
 *             Ps = 3 0  -> Don't show scrollbar (rxvt).
 *             Ps = 3 5  -> Disable font-shifting functions (rxvt).
 *             Ps = 4 0  -> Disallow 80 -> 132 Mode, xterm.
 *             Ps = 4 1  -> No more(1) fix (see curses resource).
 *             Ps = 4 2  -> Disable National Replacement Character sets
 *           (DECNRCM), VT220.
 *             Ps = 4 4  -> Turn Off Margin Bell, xterm.
 *             Ps = 4 5  -> No Reverse-wraparound Mode, xterm.
 *             Ps = 4 6  -> Stop Logging, xterm.  This is normally disabled
 *           by a compile-time option.
 *             Ps = 4 7  -> Use Normal Screen Buffer, xterm.
 *             Ps = 6 6  -> Numeric keypad (DECNKM), VT320.
 *             Ps = 6 7  -> Backarrow key sends delete (DECBKM), VT340,
 *           VT420.  This sets the backarrowKey resource to "false".
 *             Ps = 6 9  -> Disable left and right margin mode (DECLRMM),
 *           VT420 and up.
 *             Ps = 8 0  -> Disable Sixel Scrolling (DECSDM).
 *             Ps = 9 5  -> Clear screen when DECCOLM is set/reset (DEC-
 *           NCSM), VT510 and up.
 *             Ps = 1 0 0 0  -> Don't send Mouse X & Y on button press and
 *           release.  See the section Mouse Tracking.
 *             Ps = 1 0 0 1  -> Don't use Hilite Mouse Tracking, xterm.
 *             Ps = 1 0 0 2  -> Don't use Cell Motion Mouse Tracking,
 *           xterm.  See the section Button-event tracking.
 *             Ps = 1 0 0 3  -> Don't use All Motion Mouse Tracking, xterm.
 *           See the section Any-event tracking.
 *             Ps = 1 0 0 4  -> Don't send FocusIn/FocusOut events, xterm.
 *             Ps = 1 0 0 5  -> Disable UTF-8 Mouse Mode, xterm.
 *             Ps = 1 0 0 6  -> Disable SGR Mouse Mode, xterm.
 *             Ps = 1 0 0 7  -> Disable Alternate Scroll Mode, xterm.  This
 *           corresponds to the alternateScroll resource.
 *             Ps = 1 0 1 0  -> Don't scroll to bottom on tty output
 *           (rxvt).  This sets the scrollTtyOutput resource to "false".
 *             Ps = 1 0 1 1  -> Don't scroll to bottom on key press (rxvt).
 *           This sets the scrollKey resource to "false".
 *             Ps = 1 0 1 5  -> Disable urxvt Mouse Mode.
 *             Ps = 1 0 3 4  -> Don't interpret "meta" key, xterm.  This
 *           disables the eightBitInput resource.
 *             Ps = 1 0 3 5  -> Disable special modifiers for Alt and Num-
 *           Lock keys, xterm.  This disables the numLock resource.
 *             Ps = 1 0 3 6  -> Don't send ESC  when Meta modifies a key,
 *           xterm.  This disables the metaSendsEscape resource.
 *             Ps = 1 0 3 7  -> Send VT220 Remove from the editing-keypad
 *           Delete key, xterm.
 *             Ps = 1 0 3 9  -> Don't send ESC when Alt modifies a key,
 *           xterm.  This disables the altSendsEscape resource.
 *             Ps = 1 0 4 0  -> Do not keep selection when not highlighted,
 *           xterm.  This disables the keepSelection resource.
 *             Ps = 1 0 4 1  -> Use the PRIMARY selection, xterm.  This
 *           disables the selectToClipboard resource.
 *             Ps = 1 0 4 2  -> Disable Urgency window manager hint when
 *           Control-G is received, xterm.  This disables the bellIsUrgent
 *           resource.
 *             Ps = 1 0 4 3  -> Disable raising of the window when Control-
 *           G is received, xterm.  This disables the popOnBell resource.
 *             Ps = 1 0 4 6  -> Disable switching to/from Alternate Screen
 *           Buffer, xterm.  This works for terminfo-based systems, updat-
 *           ing the titeInhibit resource.  If currently using the Alter-
 *           nate Screen Buffer, xterm switches to the Normal Screen Buf-
 *           fer.
 *             Ps = 1 0 4 7  -> Use Normal Screen Buffer, xterm.  Clear the
 *           screen first if in the Alternate Screen Buffer.  This may be
 *           disabled by the titeInhibit resource.
 *             Ps = 1 0 4 8  -> Restore cursor as in DECRC, xterm.  This
 *           may be disabled by the titeInhibit resource.
 *             Ps = 1 0 4 9  -> Use Normal Screen Buffer and restore cursor
 *           as in DECRC, xterm.  This may be disabled by the titeInhibit
 *           resource.  This combines the effects of the 1 0 4 7  and 1 0 4
 *           8  modes.  Use this with terminfo-based applications rather
 *           than the 4 7  mode.
 *             Ps = 1 0 5 0  -> Reset terminfo/termcap function-key mode,
 *           xterm.
 *             Ps = 1 0 5 1  -> Reset Sun function-key mode, xterm.
 *             Ps = 1 0 5 2  -> Reset HP function-key mode, xterm.
 *             Ps = 1 0 5 3  -> Reset SCO function-key mode, xterm.
 *             Ps = 1 0 6 0  -> Reset legacy keyboard emulation, i.e,
 *           X11R6, xterm.
 *             Ps = 1 0 6 1  -> Reset keyboard emulation to Sun/PC style,
 *           xterm.
 *             Ps = 2 0 0 4  -> Reset bracketed paste mode, xterm.
 *
 * CSI Pm m  Character Attributes (SGR).
 *             Ps = 0  -> Normal (default), VT100.
 *             Ps = 1  -> Bold, VT100.
 *             Ps = 2  -> Faint, decreased intensity, ECMA-48 2nd.
 *             Ps = 3  -> Italicized, ECMA-48 2nd.
 *             Ps = 4  -> Underlined, VT100.
 *             Ps = 5  -> Blink, VT100.
 *           This appears as Bold in X11R6 xterm.
 *             Ps = 7  -> Inverse, VT100.
 *             Ps = 8  -> Invisible, i.e., hidden, ECMA-48 2nd, VT300.
 *             Ps = 9  -> Crossed-out characters, ECMA-48 3rd.
 *             Ps = 2 1  -> Doubly-underlined, ECMA-48 3rd.
 *             Ps = 2 2  -> Normal (neither bold nor faint), ECMA-48 3rd.
 *             Ps = 2 3  -> Not italicized, ECMA-48 3rd.
 *             Ps = 2 4  -> Not underlined, ECMA-48 3rd.
 *             Ps = 2 5  -> Steady (not blinking), ECMA-48 3rd.
 *             Ps = 2 7  -> Positive (not inverse), ECMA-48 3rd.
 *             Ps = 2 8  -> Visible, i.e., not hidden, ECMA-48 3rd, VT300.
 *             Ps = 2 9  -> Not crossed-out, ECMA-48 3rd.
 *             Ps = 3 0  -> Set foreground color to Black.
 *             Ps = 3 1  -> Set foreground color to Red.
 *             Ps = 3 2  -> Set foreground color to Green.
 *             Ps = 3 3  -> Set foreground color to Yellow.
 *             Ps = 3 4  -> Set foreground color to Blue.
 *             Ps = 3 5  -> Set foreground color to Magenta.
 *             Ps = 3 6  -> Set foreground color to Cyan.
 *             Ps = 3 7  -> Set foreground color to White.
 *             Ps = 3 9  -> Set foreground color to default, ECMA-48 3rd.
 *             Ps = 4 0  -> Set background color to Black.
 *             Ps = 4 1  -> Set background color to Red.
 *             Ps = 4 2  -> Set background color to Green.
 *             Ps = 4 3  -> Set background color to Yellow.
 *             Ps = 4 4  -> Set background color to Blue.
 *             Ps = 4 5  -> Set background color to Magenta.
 *             Ps = 4 6  -> Set background color to Cyan.
 *             Ps = 4 7  -> Set background color to White.
 *             Ps = 4 9  -> Set background color to default, ECMA-48 3rd.
 *
 *           Some of the above note the edition of ECMA-48 which first
 *           describes a feature.  In its successive editions from 1979 to
 *           1991 (2nd 1979, 3rd 1984, 4th 1986, and 5th 1991), ECMA-48
 *           listed codes through 6 5 (skipping several toward the end of
 *           the range).  Most of the ECMA-48 codes not implemented in
 *           xterm were never implemented in a hardware terminal.  Several
 *           (such as 3 9  and 4 9 ) are either noted in ECMA-48 as imple-
 *           mentation defined, or described in vague terms.
 *
 *           The successive editions of ECMA-48 give little attention to
 *           changes from one edition to the next, except to comment on
 *           features which have become obsolete.  ECMA-48 1st (1976) is
 *           unavailable; there is no reliable source of information which
 *           states whether "ANSI" color was defined in that edition, or
 *           later (1979).  The VT100 (1978) implemented the most commonly
 *           used non-color video attributes which are given in the 2nd
 *           edition.
 *
 *           While 8-color support is described in ECMA-48 2nd edition, the
 *           VT500 series (introduced in 1993) were the first DEC terminals
 *           implementing "ANSI" color.  The DEC terminal's use of color is
 *           known to differ from xterm; useful documentation on this
 *           series became available too late to influence xterm.
 *
 *           If 16-color support is compiled, the following aixterm con-
 *           trols apply.  Assume that xterm's resources are set so that
 *           the ISO color codes are the first 8 of a set of 16.  Then the
 *           aixterm colors are the bright versions of the ISO colors:
 *
 *             Ps = 9 0  -> Set foreground color to Black.
 *             Ps = 9 1  -> Set foreground color to Red.
 *             Ps = 9 2  -> Set foreground color to Green.
 *             Ps = 9 3  -> Set foreground color to Yellow.
 *             Ps = 9 4  -> Set foreground color to Blue.
 *             Ps = 9 5  -> Set foreground color to Magenta.
 *             Ps = 9 6  -> Set foreground color to Cyan.
 *             Ps = 9 7  -> Set foreground color to White.
 *             Ps = 1 0 0  -> Set background color to Black.
 *             Ps = 1 0 1  -> Set background color to Red.
 *             Ps = 1 0 2  -> Set background color to Green.
 *             Ps = 1 0 3  -> Set background color to Yellow.
 *             Ps = 1 0 4  -> Set background color to Blue.
 *             Ps = 1 0 5  -> Set background color to Magenta.
 *             Ps = 1 0 6  -> Set background color to Cyan.
 *             Ps = 1 0 7  -> Set background color to White.
 *
 *           If xterm is compiled with the 16-color support disabled, it
 *           supports the following, from rxvt:
 *             Ps = 1 0 0  -> Set foreground and background color to
 *           default.
 *
 *           XTerm maintains a color palette whose entries are identified
 *           by an index beginning with zero.  If 88- or 256-color support
 *           is compiled, the following apply:
 *           o   All parameters are decimal integers.
 *           o   RGB values range from zero (0) to 255.
 *           o   ISO-8613-6 has been interpreted in more than one way;
 *               xterm allows the semicolons separating the subparameters
 *               in this control to be replaced by colons (but after the
 *               first colon, colons must be used).
 *
 *           These ISO-8613-6 controls (marked in ECMA-48 5th edition as
 *           "reserved for future standardization") are supported by xterm:
 *             Pm = 3 8 ; 2 ; Pi ; Pr ; Pg ; Pb -> Set foreground color
 *           using RGB values.  If xterm is not compiled with direct-color
 *           support, it uses the closest match in its palette for the
 *           given RGB Pr/Pg/Pb.  The color space identifier Pi is ignored.
 *             Pm = 3 8 ; 5 ; Ps -> Set foreground color to Ps, using
 *           indexed color.
 *             Pm = 4 8 ; 2 ; Pi ; Pr ; Pg ; Pb -> Set background color
 *           using RGB values.  If xterm is not compiled with direct-color
 *           support, it uses the closest match in its palette for the
 *           given RGB Pr/Pg/Pb.  The color space identifier Pi is ignored.
 *             Pm = 4 8 ; 5 ; Ps -> Set background color to Ps, using
 *           indexed color.
 *
 *           This variation on ISO-8613-6 is supported for compatibility
 *           with KDE konsole:
 *             Pm = 3 8 ; 2 ; Pr ; Pg ; Pb -> Set foreground color using
 *           RGB values.  If xterm is not compiled with direct-color sup-
 *           port, it uses the closest match in its palette for the given
 *           RGB Pr/Pg/Pb.
 *             Pm = 4 8 ; 2 ; Pr ; Pg ; Pb -> Set background color using
 *           RGB values.  If xterm is not compiled with direct-color sup-
 *           port, it uses the closest match in its palette for the given
 *           RGB Pr/Pg/Pb.
 *
 *           In each case, if xterm is compiled with direct-color support,
 *           and the resource directColor is true, then rather than choos-
 *           ing the closest match, xterm asks the X server to directly
 *           render a given color.
 *
 * CSI > Pp ; Pv m
 * CSI > Pp m
 *           Set/reset key modifier options, xterm.  Set or reset resource-
 *           values used by xterm to decide whether to construct escape
 *           sequences holding information about the modifiers pressed with
 *           a given key.
 *
 *           The first parameter Pp identifies the resource to set/reset.
 *           The second parameter Pv is the value to assign to the
 *           resource.
 *
 *           If the second parameter is omitted, the resource is reset to
 *           its initial value.  Values 3  and 5  are reserved for keypad-
 *           keys and string-keys.
 *
 *             Pp = 0  -> modifyKeyboard.
 *             Pp = 1  -> modifyCursorKeys.
 *             Pp = 2  -> modifyFunctionKeys.
 *             Pp = 4  -> modifyOtherKeys.
 *
 *           If no parameters are given, all resources are reset to their
 *           initial values.
 *
 * CSI Ps n  Device Status Report (DSR).
 *             Ps = 5  -> Status Report.
 *           Result ("OK") is CSI 0 n
 *             Ps = 6  -> Report Cursor Position (CPR) [row;column].
 *           Result is CSI r ; c R
 *
 *           Note: it is possible for this sequence to be sent by a func-
 *           tion key.  For example, with the default keyboard configura-
 *           tion the shifted F1 key may send (with shift-, control-, alt-
 *           modifiers)
 *
 *             CSI 1 ; 2  R , or
 *             CSI 1 ; 5  R , or
 *             CSI 1 ; 6  R , etc.
 *
 *           The second parameter encodes the modifiers; values range from
 *           2 to 16.  See the section PC-Style Function Keys for the
 *           codes.  The modifyFunctionKeys and modifyKeyboard resources
 *           can change the form of the string sent from the modified F1
 *           key.
 *
 * CSI > Pm n
 *           Disable key modifier options, xterm.  These modifiers may be
 *           enabled via the CSI > Pm m sequence.  This control sequence
 *           corresponds to a resource value of "-1", which cannot be set
 *           with the other sequence.
 *
 *           The parameter identifies the resource to be disabled:
 *
 *             Ps = 0  -> modifyKeyboard.
 *             Ps = 1  -> modifyCursorKeys.
 *             Ps = 2  -> modifyFunctionKeys.
 *             Ps = 4  -> modifyOtherKeys.
 *
 *           If the parameter is omitted, modifyFunctionKeys is disabled.
 *           When modifyFunctionKeys is disabled, xterm uses the modifier
 *           keys to make an extended sequence of function keys rather than
 *           adding a parameter to each function key to denote the modi-
 *           fiers.
 *
 * CSI ? Ps n
 *           Device Status Report (DSR, DEC-specific).
 *             Ps = 6  -> Report Cursor Position (DECXCPR).  The response
 *           [row;column] is returned as
 *           CSI ? r ; c R
 *           (assumes the default page, i.e., "1").
 *             Ps = 1 5  -> Report Printer status.  The response is
 *           CSI ? 1 0 n  (ready).  or
 *           CSI ? 1 1 n  (not ready).
 *             Ps = 2 5  -> Report UDK status.  The response is
 *           CSI ? 2 0 n  (unlocked)
 *           or
 *           CSI ? 2 1 n  (locked).
 *             Ps = 2 6  -> Report Keyboard status.  The response is
 *           CSI ? 2 7 ; 1 ; 0 ; 0 n  (North American).
 *
 *           The last two parameters apply to VT300 & up (keyboard ready)
 *           and VT400 & up (LK01) respectively.
 *
 *             Ps = 5 3  -> Report Locator status.  The response is CSI ? 5
 *           3 n  Locator available, if compiled-in, or CSI ? 5 0 n  No
 *           Locator, if not.
 *             Ps = 5 5  -> Report Locator status.  The response is CSI ? 5
 *           3 n  Locator available, if compiled-in, or CSI ? 5 0 n  No
 *           Locator, if not.
 *             Ps = 5 6  -> Report Locator type.  The response is CSI ? 5 7
 *           ; 1 n  Mouse, if compiled-in, or CSI ? 5 7 ; 0 n  Cannot iden-
 *           tify, if not.
 *             Ps = 6 2  -> Report macro space (DECMSR).  The response is
 *           CSI Pn *  { .
 *             Ps = 6 3  -> Report memory checksum (DECCKSR), VT420 and up.
 *           The response is DCS Pt ! ~ x x x x ST .
 *               Pt is the request id (from an optional parameter to the
 *           request).
 *               The x's are hexadecimal digits 0-9 and A-F.
 *             Ps = 7 5  -> Report data integrity.  The response is CSI ? 7
 *           0 n  (ready, no errors).
 *             Ps = 8 5  -> Report multi-session configuration.  The
 *           response is CSI ? 8 3 n  (not configured for multiple-session
 *           operation).
 *
 * CSI > Ps p
 *           Set resource value pointerMode.  This is used by xterm to
 *           decide whether to hide the pointer cursor as the user types.
 *
 *           Valid values for the parameter:
 *             Ps = 0  -> never hide the pointer.
 *             Ps = 1  -> hide if the mouse tracking mode is not enabled.
 *             Ps = 2  -> always hide the pointer, except when leaving the
 *           window.
 *             Ps = 3  -> always hide the pointer, even if leaving/entering
 *           the window.
 *
 *           If no parameter is given, xterm uses the default, which is 1 .
 *
 * CSI ! p   Soft terminal reset (DECSTR), VT220 and up.
 *
 * CSI Pl ; Pc " p
 *           Set conformance level (DECSCL), VT220 and up.
 *
 *           The first parameter selects the conformance level.  Valid val-
 *           ues are:
 *             Pl = 6 1  -> level 1, e.g., VT100.
 *             Pl = 6 2  -> level 2, e.g., VT200.
 *             Pl = 6 3  -> level 3, e.g., VT300.
 *             Pl = 6 4  -> level 4, e.g., VT400.
 *             Pl = 6 5  -> level 5, e.g., VT500.
 *
 *           The second parameter selects the C1 control transmission mode.
 *           This is an optional parameter, ignored in conformance level 1.
 *           Valid values are:
 *             Pc = 0  -> 8-bit controls.
 *             Pc = 1  -> 7-bit controls (DEC factory default).
 *             Pc = 2  -> 8-bit controls.
 *
 *           The 7-bit and 8-bit control modes can also be set by S7C1T and
 *           S8C1T, but DECSCL is preferred.
 *
 * CSI Ps $ p
 *           Request ANSI mode (DECRQM).  For VT300 and up, reply DECRPM is
 *             CSI Ps; Pm$ y
 *           where Ps is the mode number as in SM/RM, and Pm is the mode
 *           value:
 *             0 - not recognized
 *             1 - set
 *             2 - reset
 *             3 - permanently set
 *             4 - permanently reset
 *
 * CSI ? Ps $ p
 *           Request DEC private mode (DECRQM).  For VT300 and up, reply
 *           DECRPM is
 *             CSI ? Ps; Pm$ y
 *           where Ps is the mode number as in DECSET/DECSET, Pm is the
 *           mode value as in the ANSI DECRQM.
 *           Two private modes are read-only (i.e., 1 3  and 1 4 ), pro-
 *           vided only for reporting their values using this control
 *           sequence.  They correspond to the resources cursorBlink and
 *           cursorBlinkXOR.
 * CSI # p
 * CSI Pm # p
 *           Push video attributes onto stack (XTPUSHSGR), xterm.  This is
 *           an alias for CSI # { , used to work around language limita-
 *           tions of C#.
 *
 * CSI > Ps q
 *           Ps = 0  -> Report xterm name and version.  The response is a
 *           DSR sequence identifying the version: DCS > | text ST
 *
 * CSI Ps q  Load LEDs (DECLL), VT100.
 *             Ps = 0  -> Clear all LEDS (default).
 *             Ps = 1  -> Light Num Lock.
 *             Ps = 2  -> Light Caps Lock.
 *             Ps = 3  -> Light Scroll Lock.
 *             Ps = 2 1  -> Extinguish Num Lock.
 *             Ps = 2 2  -> Extinguish Caps Lock.
 *             Ps = 2 3  -> Extinguish Scroll Lock.
 *
 * CSI Ps SP q
 *           Set cursor style (DECSCUSR), VT520.
 *             Ps = 0  -> blinking block.
 *             Ps = 1  -> blinking block (default).
 *             Ps = 2  -> steady block.
 *             Ps = 3  -> blinking underline.
 *             Ps = 4  -> steady underline.
 *             Ps = 5  -> blinking bar, xterm.
 *             Ps = 6  -> steady bar, xterm.
 *
 * CSI Ps " q
 *           Select character protection attribute (DECSCA).  Valid values
 *           for the parameter:
 *             Ps = 0  -> DECSED and DECSEL can erase (default).
 *             Ps = 1  -> DECSED and DECSEL cannot erase.
 *             Ps = 2  -> DECSED and DECSEL can erase.
 *
 * CSI # q   Pop video attributes from stack (XTPOPSGR), xterm.  This is an
 *           alias for CSI # } , used to work around language limitations
 *           of C#.
 *
 * CSI Ps ; Ps r
 *           Set Scrolling Region [top;bottom] (default = full size of win-
 *           dow) (DECSTBM), VT100.
 *
 * CSI ? Pm r
 *           Restore DEC Private Mode Values.  The value of Ps previously
 *           saved is restored.  Ps values are the same as for DECSET.
 *
 * CSI Pt ; Pl ; Pb ; Pr ; Ps $ r
 *           Change Attributes in Rectangular Area (DECCARA), VT400 and up.
 *             Pt ; Pl ; Pb ; Pr denotes the rectangle.
 *             Ps denotes the SGR attributes to change: 0, 1, 4, 5, 7.
 *
 * CSI s     Save cursor, available only when DECLRMM is disabled (SCOSC,
 *           also ANSI.SYS).
 *
 * CSI Pl ; Pr s
 *           Set left and right margins (DECSLRM), VT420 and up.  This is
 *           available only when DECLRMM is enabled.
 *
 * CSI ? Pm s
 *           Save DEC Private Mode Values.  Ps values are the same as for
 *           DECSET.
 *
 * CSI Ps ; Ps ; Ps t
 *           Window manipulation (from dtterm, as well as extensions by
 *           xterm).  These controls may be disabled using the allowWin-
 *           dowOps resource.
 *
 *           xterm uses Extended Window Manager Hints (EWMH) to maximize
 *           the window.  Some window managers have incomplete support for
 *           EWMH.  For instance, fvwm, flwm and quartz-wm advertise sup-
 *           port for maximizing windows horizontally or vertically, but in
 *           fact equate those to the maximize operation.
 *
 *           Valid values for the first (and any additional parameters)
 *           are:
 *             Ps = 1  -> De-iconify window.
 *             Ps = 2  -> Iconify window.
 *             Ps = 3 ;  x ;  y -> Move window to [x, y].
 *             Ps = 4 ;  height ;  width -> Resize the xterm window to
 *           given height and width in pixels.  Omitted parameters reuse
 *           the current height or width.  Zero parameters use the dis-
 *           play's height or width.
 *             Ps = 5  -> Raise the xterm window to the front of the stack-
 *           ing order.
 *             Ps = 6  -> Lower the xterm window to the bottom of the
 *           stacking order.
 *             Ps = 7  -> Refresh the xterm window.
 *             Ps = 8 ;  height ;  width -> Resize the text area to given
 *           height and width in characters.  Omitted parameters reuse the
 *           current height or width.  Zero parameters use the display's
 *           height or width.
 *             Ps = 9 ;  0  -> Restore maximized window.
 *             Ps = 9 ;  1  -> Maximize window (i.e., resize to screen
 *           size).
 *             Ps = 9 ;  2  -> Maximize window vertically.
 *             Ps = 9 ;  3  -> Maximize window horizontally.
 *             Ps = 1 0 ;  0  -> Undo full-screen mode.
 *             Ps = 1 0 ;  1  -> Change to full-screen.
 *             Ps = 1 0 ;  2  -> Toggle full-screen.
 *             Ps = 1 1  -> Report xterm window state.
 *           If the xterm window is non-iconified, it returns CSI 1 t .
 *           If the xterm window is iconified, it returns CSI 2 t .
 *             Ps = 1 3  -> Report xterm window position.
 *           Note: X Toolkit positions can be negative, but the reported
 *           values are unsigned, in the range 0-65535.  Negative values
 *           correspond to 32768-65535.
 *           Result is CSI 3 ; x ; y t
 *             Ps = 1 3 ;  2  -> Report xterm text-area position.
 *           Result is CSI 3 ; x ; y t
 *             Ps = 1 4  -> Report xterm text area size in pixels.
 *           Result is CSI  4 ;  height ;  width t
 *             Ps = 1 4 ;  2  -> Report xterm window size in pixels.
 *           Normally xterm's window is larger than its text area, since it
 *           includes the frame (or decoration) applied by the window man-
 *           ager, as well as the area used by a scroll-bar.
 *           Result is CSI  4 ;  height ;  width t
 *             Ps = 1 5  -> Report size of the screen in pixels.
 *           Result is CSI  5 ;  height ;  width t
 *             Ps = 1 6  -> Report xterm character cell size in pixels.
 *           Result is CSI  6 ;  height ;  width t
 *             Ps = 1 8  -> Report the size of the text area in characters.
 *           Result is CSI  8 ;  height ;  width t
 *             Ps = 1 9  -> Report the size of the screen in characters.
 *           Result is CSI  9 ;  height ;  width t
 *             Ps = 2 0  -> Report xterm window's icon label.
 *           Result is OSC  L  label ST
 *             Ps = 2 1  -> Report xterm window's title.
 *           Result is OSC  l  label ST
 *             Ps = 2 2 ; 0  -> Save xterm icon and window title on stack.
 *             Ps = 2 2 ; 1  -> Save xterm icon title on stack.
 *             Ps = 2 2 ; 2  -> Save xterm window title on stack.
 *             Ps = 2 3 ; 0  -> Restore xterm icon and window title from
 *           stack.
 *             Ps = 2 3 ; 1  -> Restore xterm icon title from stack.
 *             Ps = 2 3 ; 2  -> Restore xterm window title from stack.
 *             Ps >= 2 4  -> Resize to Ps lines (DECSLPP), VT340 and VT420.
 *           xterm adapts this by resizing its window.
 *
 * CSI > Pm t
 *           This xterm control sets one or more features of the title
 *           modes.  Each parameter enables a single feature.
 *             Ps = 0  -> Set window/icon labels using hexadecimal.
 *             Ps = 1  -> Query window/icon labels using hexadecimal.
 *             Ps = 2  -> Set window/icon labels using UTF-8.
 *             Ps = 3  -> Query window/icon labels using UTF-8.  (See dis-
 *           cussion of Title Modes)
 *
 * CSI Ps SP t
 *           Set warning-bell volume (DECSWBV), VT520.
 *             Ps = 0  or 1  -> off.
 *             Ps = 2 , 3  or 4  -> low.
 *             Ps = 5 , 6 , 7 , or 8  -> high.
 *
 * CSI Pt ; Pl ; Pb ; Pr ; Ps $ t
 *           Reverse Attributes in Rectangular Area (DECRARA), VT400 and
 *           up.
 *             Pt ; Pl ; Pb ; Pr denotes the rectangle.
 *             Ps denotes the attributes to reverse, i.e.,  1, 4, 5, 7.
 *
 * CSI u     Restore cursor (SCORC, also ANSI.SYS).
 *
 * CSI Ps SP u
 *           Set margin-bell volume (DECSMBV), VT520.
 *             Ps = 0 , 5 , 6 , 7 , or 8  -> high.
 *             Ps = 1  -> off.
 *             Ps = 2 , 3  or 4  -> low.
 *
 * CSI Pt ; Pl ; Pb ; Pr ; Pp ; Pt ; Pl ; Pp $ v
 *           Copy Rectangular Area (DECCRA), VT400 and up.
 *             Pt ; Pl ; Pb ; Pr denotes the rectangle.
 *             Pp denotes the source page.
 *             Pt ; Pl denotes the target location.
 *             Pp denotes the target page.
 *
 * CSI Ps $ w
 *           Request presentation state report (DECRQPSR), VT320 and up.
 *             Ps = 0  -> error.
 *             Ps = 1  -> cursor information report (DECCIR).
 *           Response is
 *             DCS 1 $ u Pt ST
 *           Refer to the VT420 programming manual, which requires six
 *           pages to document the data string Pt,
 *             Ps = 2  -> tab stop report (DECTABSR).
 *           Response is
 *             DCS 2 $ u Pt ST
 *           The data string Pt is a list of the tab-stops, separated by
 *           "/" characters.
 *
 * CSI Pt ; Pl ; Pb ; Pr ' w
 *           Enable Filter Rectangle (DECEFR), VT420 and up.
 *           Parameters are [top;left;bottom;right].
 *           Defines the coordinates of a filter rectangle and activates
 *           it.  Anytime the locator is detected outside of the filter
 *           rectangle, an outside rectangle event is generated and the
 *           rectangle is disabled.  Filter rectangles are always treated
 *           as "one-shot" events.  Any parameters that are omitted default
 *           to the current locator position.  If all parameters are omit-
 *           ted, any locator motion will be reported.  DECELR always can-
 *           cels any previous rectangle definition.
 *
 * CSI Ps x  Request Terminal Parameters (DECREQTPARM).
 *           if Ps is a "0" (default) or "1", and xterm is emulating VT100,
 *           the control sequence elicits a response of the same form whose
 *           parameters describe the terminal:
 *             Ps -> the given Ps incremented by 2.
 *             Pn = 1  <- no parity.
 *             Pn = 1  <- eight bits.
 *             Pn = 1  <- 2 8  transmit 38.4k baud.
 *             Pn = 1  <- 2 8  receive 38.4k baud.
 *             Pn = 1  <- clock multiplier.
 *             Pn = 0  <- STP flags.
 *
 * CSI Ps * x
 *           Select Attribute Change Extent (DECSACE), VT420 and up.
 *             Ps = 0  -> from start to end position, wrapped.
 *             Ps = 1  -> from start to end position, wrapped.
 *             Ps = 2  -> rectangle (exact).
 *
 * CSI Pc ; Pt ; Pl ; Pb ; Pr $ x
 *           Fill Rectangular Area (DECFRA), VT420 and up.
 *             Pc is the character to use.
 *             Pt ; Pl ; Pb ; Pr denotes the rectangle.
 *
 * CSI Ps # y
 *           Select checksum extension (XTCHECKSUM), xterm.  The bits of Ps
 *           modify the calculation of the checksum returned by DECRQCRA:
 *             0  -> do not negate the result.
 *             1  -> do not report the VT100 video attributes.
 *             2  -> do not omit checksum for blanks.
 *             3  -> omit checksum for cells not explicitly initialized.
 *             4  -> do not mask cell value to 8 bits or ignore combining
 *           characters.
 *             5  -> do not mask cell value to 7 bits.
 *
 * CSI Pi ; Pg ; Pt ; Pl ; Pb ; Pr * y
 *           Request Checksum of Rectangular Area (DECRQCRA), VT420 and up.
 *           Response is
 *           DCS Pi ! ~ x x x x ST
 *             Pi is the request id.
 *             Pg is the page number.
 *             Pt ; Pl ; Pb ; Pr denotes the rectangle.
 *             The x's are hexadecimal digits 0-9 and A-F.
 *
 * CSI Ps ; Pu ' z
 *           Enable Locator Reporting (DECELR).
 *           Valid values for the first parameter:
 *             Ps = 0  -> Locator disabled (default).
 *             Ps = 1  -> Locator enabled.
 *             Ps = 2  -> Locator enabled for one report, then disabled.
 *           The second parameter specifies the coordinate unit for locator
 *           reports.
 *           Valid values for the second parameter:
 *             Pu = 0  or omitted -> default to character cells.
 *             Pu = 1  <- device physical pixels.
 *             Pu = 2  <- character cells.
 *
 * CSI Pt ; Pl ; Pb ; Pr $ z
 *           Erase Rectangular Area (DECERA), VT400 and up.
 *             Pt ; Pl ; Pb ; Pr denotes the rectangle.
 *
 * CSI Pm ' {
 *           Select Locator Events (DECSLE).
 *           Valid values for the first (and any additional parameters)
 *           are:
 *             Ps = 0  -> only respond to explicit host requests (DECRQLP).
 *           This is default.  It also cancels any filter rectangle.
 *             Ps = 1  -> report button down transitions.
 *             Ps = 2  -> do not report button down transitions.
 *             Ps = 3  -> report button up transitions.
 *             Ps = 4  -> do not report button up transitions.
 *
 * CSI # {
 * CSI Pm # {
 *           Push video attributes onto stack (XTPUSHSGR), xterm.  The
 *           optional parameters correspond to the SGR encoding for video
 *           attributes, except for colors (which do not have a unique SGR
 *           code):
 *             Ps = 1  -> Bold.
 *             Ps = 2  -> Faint.
 *             Ps = 3  -> Italicized.
 *             Ps = 4  -> Underlined.
 *             Ps = 5  -> Blink.
 *             Ps = 7  -> Inverse.
 *             Ps = 8  -> Invisible.
 *             Ps = 9  -> Crossed-out characters.
 *             Ps = 1 0  -> Foreground color.
 *             Ps = 1 1  -> Background color.
 *             Ps = 2 1  -> Doubly-underlined.
 *
 *           If no parameters are given, all of the video attributes are
 *           saved.  The stack is limited to 10 levels.
 *
 * CSI Pt ; Pl ; Pb ; Pr $ {
 *           Selective Erase Rectangular Area (DECSERA), VT400 and up.
 *             Pt ; Pl ; Pb ; Pr denotes the rectangle.
 *
 * CSI Pt ; Pl ; Pb ; Pr # |
 *           Report selected graphic rendition (XTREPORTSGR), xterm.  The
 *           response is an SGR sequence which contains the attributes
 *           which are common to all cells in a rectangle.
 *             Pt ; Pl ; Pb ; Pr denotes the rectangle.
 *
 * CSI Ps $ |
 *           Select columns per page (DECSCPP), VT340.
 *             Ps = 0  -> 80 columns, default if Ps omitted.
 *             Ps = 8 0  -> 80 columns.
 *             Ps = 1 3 2  -> 132 columns.
 *
 * CSI Ps ' |
 *           Request Locator Position (DECRQLP).
 *           Valid values for the parameter are:
 *             Ps = 0 , 1 or omitted -> transmit a single DECLRP locator
 *           report.
 *
 *           If Locator Reporting has been enabled by a DECELR, xterm will
 *           respond with a DECLRP Locator Report.  This report is also
 *           generated on button up and down events if they have been
 *           enabled with a DECSLE, or when the locator is detected outside
 *           of a filter rectangle, if filter rectangles have been enabled
 *           with a DECEFR.
 *
 *             <- CSI Pe ; Pb ; Pr ; Pc ; Pp &  w
 *
 *           Parameters are [event;button;row;column;page].
 *           Valid values for the event:
 *             Pe = 0  <- locator unavailable - no other parameters sent.
 *             Pe = 1  <- request - xterm received a DECRQLP.
 *             Pe = 2  <- left button down.
 *             Pe = 3  <- left button up.
 *             Pe = 4  <- middle button down.
 *             Pe = 5  <- middle button up.
 *             Pe = 6  <- right button down.
 *             Pe = 7  <- right button up.
 *             Pe = 8  <- M4 button down.
 *             Pe = 9  <- M4 button up.
 *             Pe = 1 0  <- locator outside filter rectangle.
 *           The "button" parameter is a bitmask indicating which buttons
 *           are pressed:
 *             Pb = 0  <- no buttons down.
 *             Pb & 1  <- right button down.
 *             Pb & 2  <- middle button down.
 *             Pb & 4  <- left button down.
 *             Pb & 8  <- M4 button down.
 *           The "row" and "column" parameters are the coordinates of the
 *           locator position in the xterm window, encoded as ASCII deci-
 *           mal.
 *           The "page" parameter is not used by xterm.
 *
 * CSI Ps * |
 *           Select number of lines per screen (DECSNLS), VT420 and up.
 *
 * CSI # }   Pop video attributes from stack (XTPOPSGR), xterm.  Popping
 *           restores the video-attributes which were saved using XTPUSHSGR
 *           to their previous state.
 *
 * CSI Pm ' }
 *           Insert Ps Column(s) (default = 1) (DECIC), VT420 and up.
 *
 * CSI Pm ' ~
 *           Delete Ps Column(s) (default = 1) (DECDC), VT420 and up.
 *
 *
 * Operating System Commands
 *
 * OSC Ps ; Pt BEL
 *
 * OSC Ps ; Pt ST
 *           Set Text Parameters.  For colors and font, if Pt is a "?", the
 *           control sequence elicits a response which consists of the con-
 *           trol sequence which would set the corresponding value.  The
 *           dtterm control sequences allow you to determine the icon name
 *           and window title.
 *             Ps = 0  -> Change Icon Name and Window Title to Pt.
 *             Ps = 1  -> Change Icon Name to Pt.
 *             Ps = 2  -> Change Window Title to Pt.
 *             Ps = 3  -> Set X property on top-level window.  Pt should be
 *           in the form "prop=value", or just "prop" to delete the prop-
 *           erty.
 *             Ps = 4 ; c ; spec -> Change Color Number c to the color
 *           specified by spec.  This can be a name or RGB specification as
 *           per XParseColor.  Any number of c/spec pairs may be given.
 *           The color numbers correspond to the ANSI colors 0-7, their
 *           bright versions 8-15, and if supported, the remainder of the
 *           88-color or 256-color table.
 *
 *           If a "?" is given rather than a name or RGB specification,
 *           xterm replies with a control sequence of the same form which
 *           can be used to set the corresponding color.  Because more than
 *           one pair of color number and specification can be given in one
 *           control sequence, xterm can make more than one reply.
 *
 *             Ps = 5 ; c ; spec -> Change Special Color Number c to the
 *           color specified by spec.  This can be a name or RGB specifica-
 *           tion as per XParseColor.  Any number of c/spec pairs may be
 *           given.  The special colors can also be set by adding the maxi-
 *           mum number of colors to these codes in an OSC 4  control:
 *
 *               Pc = 0  <- resource colorBD (BOLD).
 *               Pc = 1  <- resource colorUL (UNDERLINE).
 *               Pc = 2  <- resource colorBL (BLINK).
 *               Pc = 3  <- resource colorRV (REVERSE).
 *               Pc = 4  <- resource colorIT (ITALIC).
 *
 *             Ps = 6 ; c ; f -> Enable/disable Special Color Number c.
 *           The second parameter tells xterm to enable the corresponding
 *           color mode if nonzero, disable it if zero.  OSC 6  is the same
 *           as OSC 1 0 6 .
 *
 *           The 10 colors (below) which may be set or queried using 1 0
 *           through 1 9  are denoted dynamic colors, since the correspond-
 *           ing control sequences were the first means for setting xterm's
 *           colors dynamically, i.e., after it was started.  They are not
 *           the same as the ANSI colors (however, the dynamic text fore-
 *           ground and background colors are used when ANSI colors are
 *           reset using SGR 3 9  and 4 9 , respectively).  These controls
 *           may be disabled using the allowColorOps resource.  At least
 *           one parameter is expected for Pt.  Each successive parameter
 *           changes the next color in the list.  The value of Ps tells the
 *           starting point in the list.  The colors are specified by name
 *           or RGB specification as per XParseColor.
 *
 *           If a "?" is given rather than a name or RGB specification,
 *           xterm replies with a control sequence of the same form which
 *           can be used to set the corresponding dynamic color.  Because
 *           more than one pair of color number and specification can be
 *           given in one control sequence, xterm can make more than one
 *           reply.
 *
 *             Ps = 1 0  -> Change VT100 text foreground color to Pt.
 *             Ps = 1 1  -> Change VT100 text background color to Pt.
 *             Ps = 1 2  -> Change text cursor color to Pt.
 *             Ps = 1 3  -> Change mouse foreground color to Pt.
 *             Ps = 1 4  -> Change mouse background color to Pt.
 *             Ps = 1 5  -> Change Tektronix foreground color to Pt.
 *             Ps = 1 6  -> Change Tektronix background color to Pt.
 *             Ps = 1 7  -> Change highlight background color to Pt.
 *             Ps = 1 8  -> Change Tektronix cursor color to Pt.
 *             Ps = 1 9  -> Change highlight foreground color to Pt.
 *
 *             Ps = 4 6  -> Change Log File to Pt.  This is normally dis-
 *           abled by a compile-time option.
 *
 *             Ps = 5 0  -> Set Font to Pt.  These controls may be disabled
 *           using the allowFontOps resource.  If Pt begins with a "#",
 *           index in the font menu, relative (if the next character is a
 *           plus or minus sign) or absolute.  A number is expected but not
 *           required after the sign (the default is the current entry for
 *           relative, zero for absolute indexing).
 *
 *           The same rule (plus or minus sign, optional number) is used
 *           when querying the font.  The remainder of Pt is ignored.
 *
 *           A font can be specified after a "#" index expression, by
 *           adding a space and then the font specifier.
 *
 *           If the TrueType Fonts menu entry is set (the renderFont
 *           resource), then this control sets/queries the faceName
 *           resource.
 *
 *             Ps = 5 1  -> reserved for Emacs shell.
 *
 *             Ps = 5 2  -> Manipulate Selection Data.  These controls may
 *           be disabled using the allowWindowOps resource.  The parameter
 *           Pt is parsed as
 *                Pc ; Pd
 *           The first, Pc, may contain zero or more characters from the
 *           set c , p , q , s , 0 , 1 , 2 , 3 , 4 , 5 , 6 , and 7 .  It is
 *           used to construct a list of selection parameters for clip-
 *           board, primary, secondary, select, or cut buffers 0 through 7
 *           respectively, in the order given.  If the parameter is empty,
 *           xterm uses s 0 , to specify the configurable primary/clipboard
 *           selection and cut buffer 0.
 *
 *           The second parameter, Pd, gives the selection data.  Normally
 *           this is a string encoded in base64 (RFC-4648).  The data
 *           becomes the new selection, which is then available for pasting
 *           by other applications.
 *
 *           If the second parameter is a ? , xterm replies to the host
 *           with the selection data encoded using the same protocol.  It
 *           uses the first selection found by asking successively for each
 *           item from the list of selection parameters.
 *
 *           If the second parameter is neither a base64 string nor ? ,
 *           then the selection is cleared.
 *
 *             Ps = 1 0 4 ; c -> Reset Color Number c.  It is reset to the
 *           color specified by the corresponding X resource.  Any number
 *           of c parameters may be given.  These parameters correspond to
 *           the ANSI colors 0-7, their bright versions 8-15, and if sup-
 *           ported, the remainder of the 88-color or 256-color table.  If
 *           no parameters are given, the entire table will be reset.
 *
 *             Ps = 1 0 5 ; c -> Reset Special Color Number c.  It is reset
 *           to the color specified by the corresponding X resource.  Any
 *           number of c parameters may be given.  These parameters corre-
 *           spond to the special colors which can be set using an OSC 5
 *           control (or by adding the maximum number of colors using an
 *           OSC 4  control).
 *
 *             Ps = 1 0 6 ; c ; f -> Enable/disable Special Color Number c.
 *           The second parameter tells xterm to enable the corresponding
 *           color mode if nonzero, disable it if zero.
 *
 *               Pc = 0  <- resource colorBDMode (BOLD).
 *               Pc = 1  <- resource colorULMode (UNDERLINE).
 *               Pc = 2  <- resource colorBLMode (BLINK).
 *               Pc = 3  <- resource colorRVMode (REVERSE).
 *               Pc = 4  <- resource colorITMode (ITALIC).
 *               Pc = 5  <- resource colorAttrMode (Override ANSI).
 *
 *           The dynamic colors can also be reset to their default
 *           (resource) values:
 *             Ps = 1 1 0  -> Reset VT100 text foreground color.
 *             Ps = 1 1 1  -> Reset VT100 text background color.
 *             Ps = 1 1 2  -> Reset text cursor color.
 *             Ps = 1 1 3  -> Reset mouse foreground color.
 *             Ps = 1 1 4  -> Reset mouse background color.
 *             Ps = 1 1 5  -> Reset Tektronix foreground color.
 *             Ps = 1 1 6  -> Reset Tektronix background color.
 *             Ps = 1 1 7  -> Reset highlight color.
 *             Ps = 1 1 8  -> Reset Tektronix cursor color.
 *             Ps = 1 1 9  -> Reset highlight foreground color.
 *
 *             Ps = I  ; c -> Set icon to file.  Sun shelltool, CDE dtterm.
 *           The file is expected to be XPM format, and uses the same
 *           search logic as the iconHint resource.
 *
 *             Ps = l  ; c -> Set window title.  Sun shelltool, CDE dtterm.
 *
 *             Ps = L  ; c -> Set icon label.  Sun shelltool, CDE dtterm.
 *
 *
 * Privacy Message
 *
 * PM Pt ST  xterm implements no PM  functions; Pt is ignored.  Pt need not
 *           be printable characters.
 *
 *
 * Alt and Meta Keys
 *
 * Many keyboards have keys labeled "Alt".  Few have keys labeled "Meta".
 * However, xterm's default translations use the Meta modifier.  Common
 * keyboard configurations assign the Meta modifier to an "Alt" key.  By
 * using xmodmap one may have the modifier assigned to a different key, and
 * have "real" alt and meta keys.  Here is an example:
 *
 *      ! put meta on mod3 to distinguish it from alt
 *      keycode 64 = Alt_L
 *      clear mod1
 *      add mod1 = Alt_L
 *      keycode 115 = Meta_L
 *      clear mod3
 *      add mod3 = Meta_L
 *
 *
 * The metaSendsEscape resource (and altSendsEscape if altIsNotMeta is set)
 * can be used to control the way the Meta modifier applies to ordinary
 * keys unless the modifyOtherKeys resource is set:
 *
 * o   prefix a key with the ESC  character.
 *
 * o   shift the key from codes 0-127 to 128-255 by adding 128.
 *
 * When modifyOtherKeys is set, ordinary keys may be sent as escape
 * sequences:
 *
 * o   When modifyOtherKeys is set to 1, only the alt- and meta-modifiers
 *     apply.  For example, alt-Tab sends CSI 2 7 ; 3 ; 9 ~ (the second
 *     parameter is "3" for alt, and the third parameter is the ASCII value
 *     of tab, "9").
 *
 * o   When modifyOtherKeys is set to 2, all of the modifiers apply.  For
 *     example, shift-Tab sends CSI 2 7 ; 2 ; 9 ~ rather than CSI Z (the
 *     second parameter is "2" for shift).
 *
 * The formatOtherKeys resource tells n  to change the format of the escape
 * sequences sent when modifyOtherKeys applies.  When modifyOtherKeys is
 * set to 1, for example alt-Tab sends CSI 9 ; 3 u (changing the order of
 * parameters).  One drawback to this format is that applications may con-
 * fuse it with CSI u  (restore-cursor).
 *
 * The xterm FAQ section
 *
 *    How can my program distinguish control-I from tab?
 *
 * goes into greater detail on this topic.
 *
 * The table shows the result for a given character "x" with modifiers
 * according to the default translations with the resources set on or off.
 * This assumes altIsNotMeta is set:
 *
 *        -----------------------------------------------------------
 *        key          altSendsEscape   metaSendsEscape   result
 *        -----------+----------------+-----------------+------------
 *        x          | off            | off             | x
 *        Meta-x     | off            | off             | shift
 *        Alt-x      | off            | off             | shift
 *        Alt+Meta-x | off            | off             | shift
 *        x          | ON             | off             | x
 *        Meta-x     | ON             | off             | shift
 *        Alt-x      | ON             | off             | ESC  x
 *        Alt+Meta-x | ON             | off             | ESC  shift
 *        x          | off            | ON              | x
 *        Meta-x     | off            | ON              | ESC  x
 *        Alt-x      | off            | ON              | shift
 *        Alt+Meta-x | off            | ON              | ESC  shift
 *        x          | ON             | ON              | x
 *        Meta-x     | ON             | ON              | ESC  x
 *        Alt-x      | ON             | ON              | ESC  x
 *        Alt+Meta-x | ON             | ON              | ESC  x
 *        -----------+----------------+-----------------+------------
 *
 *
 *
 * PC-Style Function Keys
 *
 * If xterm does minimal translation of the function keys, it usually does
 * this with a PC-style keyboard, so PC-style function keys result.  Sun
 * keyboards are similar to PC keyboards.  Both have cursor and scrolling
 * operations printed on the keypad, which duplicate the smaller cursor and
 * scrolling keypads.
 *
 * X does not predefine NumLock (used for VT220 keyboards) or Alt (used as
 * an extension for the Sun/PC keyboards) as modifiers.  These keys are
 * recognized as modifiers when enabled by the numLock resource, or by the
 * "DECSET 1 0 3 5 " control sequence.
 *
 * The cursor keys transmit the following escape sequences depending on the
 * mode specified via the DECCKM escape sequence.
 *
 *                   Key            Normal     Application
 *                   -------------+----------+-------------
 *                   Cursor Up    | CSI A    | SS3 A
 *                   Cursor Down  | CSI B    | SS3 B
 *                   Cursor Right | CSI C    | SS3 C
 *                   Cursor Left  | CSI D    | SS3 D
 *                   -------------+----------+-------------
 *
 * The home- and end-keys (unlike PageUp and other keys also on the 6-key
 * editing keypad) are considered "cursor keys" by xterm.  Their mode is
 * also controlled by the DECCKM escape sequence:
 *
 *                     Key        Normal     Application
 *                     ---------+----------+-------------
 *                     Home     | CSI H    | SS3 H
 *                     End      | CSI F    | SS3 F
 *                     ---------+----------+-------------
 *
 *
 * The application keypad transmits the following escape sequences depend-
 * ing on the mode specified via the DECKPNM and DECKPAM escape sequences.
 * Use the NumLock key to override the application mode.
 *
 * Not all keys are present on the Sun/PC keypad (e.g., PF1, Tab), but are
 * supported by the program.
 *
 *       Key              Numeric    Application   Terminfo   Termcap
 *       ---------------+----------+-------------+----------+----------
 *       Space          | SP       | SS3 SP      | -        | -
 *       Tab            | TAB      | SS3 I       | -        | -
 *       Enter          | CR       | SS3 M       | kent     | @8
 *       PF1            | SS3 P    | SS3 P       | kf1      | k1
 *       PF2            | SS3 Q    | SS3 Q       | kf2      | k2
 *       PF3            | SS3 R    | SS3 R       | kf3      | k3
 *       PF4            | SS3 S    | SS3 S       | kf4      | k4
 *       * (multiply)   | *        | SS3 j       | -        | -
 *       + (add)        | +        | SS3 k       | -        | -
 *       , (comma)      | ,        | SS3 l       | -        | -
 *       - (minus)      | -        | SS3 m       | -        | -
 *       . (Delete)     | .        | CSI 3 ~     | -        | -
 *       / (divide)     | /        | SS3 o       | -        | -
 *       0 (Insert)     | 0        | CSI 2 ~     | -        | -
 *       1 (End)        | 1        | SS3 F       | kc1      | K4
 *       2 (DownArrow)  | 2        | CSI B       | -        | -
 *       3 (PageDown)   | 3        | CSI 6 ~     | kc3      | K5
 *       4 (LeftArrow)  | 4        | CSI D       | -        | -
 *       5 (Begin)      | 5        | CSI E       | kb2      | K2
 *       6 (RightArrow) | 6        | CSI C       | -        | -
 *       7 (Home)       | 7        | SS3 H       | ka1      | K1
 *       8 (UpArrow)    | 8        | CSI A       | -        | -
 *       9 (PageUp)     | 9        | CSI 5 ~     | ka3      | K3
 *       = (equal)      | =        | SS3 X       | -        | -
 *       ---------------+----------+-------------+----------+----------
 *
 * They also provide 12 function keys, as well as a few other special-pur-
 * pose keys:
 *
 *                        Key        Escape Sequence
 *                        ---------+-----------------
 *                        F1       | SS3 P
 *                        F2       | SS3 Q
 *                        F3       | SS3 R
 *                        F4       | SS3 S
 *                        F5       | CSI 1 5 ~
 *                        F6       | CSI 1 7 ~
 *                        F7       | CSI 1 8 ~
 *                        F8       | CSI 1 9 ~
 *                        F9       | CSI 2 0 ~
 *                        F10      | CSI 2 1 ~
 *                        F11      | CSI 2 3 ~
 *                        F12      | CSI 2 4 ~
 *                        ---------+-----------------
 *
 *
 * Note that F1 through F4 are prefixed with SS3 , while the other keys are
 * prefixed with CSI .  Older versions of xterm implement different escape
 * sequences for F1 through F4, with a CSI  prefix.  These can be activated
 * by setting the oldXtermFKeys resource.  However, since they do not cor-
 * respond to any hardware terminal, they have been deprecated.  (The DEC
 * VT220 reserves F1 through F5 for local functions such as Setup).
 *
 *                        Key        Escape Sequence
 *                        ---------+-----------------
 *                        F1       | CSI 1 1 ~
 *                        F2       | CSI 1 2 ~
 *                        F3       | CSI 1 3 ~
 *                        F4       | CSI 1 4 ~
 *                        ---------+-----------------
 *
 * In normal mode, i.e., a Sun/PC keyboard when the sunKeyboard resource is
 * false (and none of the other keyboard resources such as oldXtermFKeys
 * resource is set), xterm encodes function key modifiers as parameters
 * appended before the final character of the control sequence.  As a spe-
 * cial case, the SS3  sent before F1 through F4 is altered to CSI  when
 * sending a function key modifier as a parameter.
 *
 *                     Code     Modifiers
 *                   ---------+---------------------------
 *                      2     | Shift
 *                      3     | Alt
 *                      4     | Shift + Alt
 *                      5     | Control
 *                      6     | Shift + Control
 *                      7     | Alt + Control
 *                      8     | Shift + Alt + Control
 *                      9     | Meta
 *                      10    | Meta + Shift
 *                      11    | Meta + Alt
 *                      12    | Meta + Alt + Shift
 *                      13    | Meta + Ctrl
 *                      14    | Meta + Ctrl + Shift
 *                      15    | Meta + Ctrl + Alt
 *                      16    | Meta + Ctrl + Alt + Shift
 *                   ---------+---------------------------
 *
 * For example, shift-F5 would be sent as CSI 1 5 ; 2 ~
 *
 * If the alwaysUseMods resource is set, the Meta modifier also is recog-
 * nized, making parameters 9 through 16.
 *
 * The codes used for the PC-style function keys were inspired by a feature
 * of the VT510, referred to in its reference manual as DECFNK.  In the
 * DECFNK scheme, codes 2-8 identify modifiers for function-keys and cur-
 * sor-, editing-keypad keys.  Unlike xterm, the VT510 limits the modifiers
 * which can be used with cursor- and editing-keypad keys.  Although the
 * name "DECFNK" implies that it is a mode, the VT510 manual mentions it
 * only as a feature, which (like xterm) interacts with the DECUDK feature.
 * Unlike xterm, VT510/VT520 provide an extension to DECUDK (DECPFK and
 * DECPAK) which apparently was the reason for the feature in those termi-
 * nals, i.e., for identifying a programmable key rather than making it
 * simple for applications to obtain modifier information.  It is not
 * described in the related VT520 manual.  Neither manual was readily
 * available at the time the feature was added to xterm.
 *
 * On the other hand, the VT510 and VT520 reference manuals do document a
 * related feature.  That is its emulation of the SCO console, which is
 * similar to the "xterm-sco" terminal description.  The SCO console func-
 * tion-keys are less useful to applications developers than the approach
 * used by xterm because
 *
 * o   the relationship between modifiers and the characters sent by func-
 *     tion-keys is not readily apparent, and
 *
 * o   the scheme is not extensible, i.e., it is an ad hoc assignment lim-
 *     ited to two modifiers (shift and control).
 *
 *
 * VT220-Style Function Keys
 *
 * However, xterm is most useful as a DEC VT102 or VT220 emulator.  Set the
 * sunKeyboard resource to true to force a Sun/PC keyboard to act like a
 * VT220 keyboard.
 *
 * The VT102/VT220 application keypad transmits unique escape sequences in
 * application mode, which are distinct from the cursor and scrolling key-
 * pad:
 *
 *             Key            Numeric    Application   VT100?
 *             -------------+----------+-------------+----------
 *             Space        | SP       | SS3 SP      | no
 *             Tab          | TAB      | SS3 I       | no
 *             Enter        | CR       | SS3 M       | yes
 *             PF1          | SS3 P    | SS3 P       | yes
 *             PF2          | SS3 Q    | SS3 Q       | yes
 *             PF3          | SS3 R    | SS3 R       | yes
 *             PF4          | SS3 S    | SS3 S       | yes
 *             * (multiply) | *        | SS3 j       | no
 *             + (add)      | +        | SS3 k       | no
 *             , (comma)    | ,        | SS3 l       | yes
 *             - (minus)    | -        | SS3 m       | yes
 *             . (period)   | .        | SS3 n       | yes
 *             / (divide)   | /        | SS3 o       | no
 *             0            | 0        | SS3 p       | yes
 *             1            | 1        | SS3 q       | yes
 *             2            | 2        | SS3 r       | yes
 *             3            | 3        | SS3 s       | yes
 *             4            | 4        | SS3 t       | yes
 *             5            | 5        | SS3 u       | yes
 *             6            | 6        | SS3 v       | yes
 *             7            | 7        | SS3 w       | yes
 *             8            | 8        | SS3 x       | yes
 *             9            | 9        | SS3 y       | yes
 *             = (equal)    | =        | SS3 X       | no
 *             -------------+----------+-------------+----------
 *
 *
 * The VT100/VT220 keypad did not have all of those keys.  They were imple-
 * mented in xterm in X11R1 (1987), defining a mapping of all X11 keys
 * which might be provided on a keypad.  For instance, a Sun4/II type-4
 * keyboard provided "=" (equal), "/" (divide), and "*" (multiply).
 *
 * While the VT420 provided the same keypad, the VT520 used a PC-keyboard.
 * Because that keyboard's keypad lacks the "," (comma), it was not possi-
 * ble to use EDT's delete-character function with the keypad.  XTerm
 * solves that problem for the VT220-keyboard configuration by mapping
 *
 *   Ctrl +  to ,  and
 *   Ctrl -  to -
 *
 * The VT220 provides a 6-key editing keypad, which is analogous to that on
 * the PC keyboard.  It is not affected by PM or DECKPNM/DECKPAM:
 *
 *                    Key        Normal     Application
 *                    ---------+----------+-------------
 *                    Insert   | CSI 2 ~  | CSI 2 ~
 *                    Delete   | CSI 3 ~  | CSI 3 ~
 *                    Home     | CSI 1 ~  | CSI 1 ~
 *                    End      | CSI 4 ~  | CSI 4 ~
 *                    PageUp   | CSI 5 ~  | CSI 5 ~
 *                    PageDown | CSI 6 ~  | CSI 6 ~
 *                    ---------+----------+-------------
 *
 *
 * The VT220 provides 8 additional function keys.  With a Sun/PC keyboard,
 * access these keys by Control/F1 for F13, etc.
 *
 *                        Key        Escape Sequence
 *                        ---------+-----------------
 *                        F13      | CSI 2 5 ~
 *                        F14      | CSI 2 6 ~
 *                        F15      | CSI 2 8 ~
 *                        F16      | CSI 2 9 ~
 *                        F17      | CSI 3 1 ~
 *                        F18      | CSI 3 2 ~
 *                        F19      | CSI 3 3 ~
 *                        F20      | CSI 3 4 ~
 *                        ---------+-----------------
 *
 *
 *
 * VT52-Style Function Keys
 *
 * A VT52 does not have function keys, but it does have a numeric keypad
 * and cursor keys.  They differ from the other emulations by the prefix.
 * Also, the cursor keys do not change:
 *
 *                    Key            Normal/Application
 *                    -------------+--------------------
 *                    Cursor Up    | ESC A
 *                    Cursor Down  | ESC B
 *                    Cursor Right | ESC C
 *                    Cursor Left  | ESC D
 *                    -------------+--------------------
 *
 * The keypad is similar:
 *
 *             Key            Numeric    Application   VT52?
 *             -------------+----------+-------------+----------
 *             Space        | SP       | ESC ? SP    | no
 *             Tab          | TAB      | ESC ? I     | no
 *             Enter        | CR       | ESC ? M     | no
 *             PF1          | ESC P    | ESC P       | yes
 *             PF2          | ESC Q    | ESC Q       | yes
 *             PF3          | ESC R    | ESC R       | yes
 *             PF4          | ESC S    | ESC S       | no
 *             * (multiply) | *        | ESC ? j     | no
 *             + (add)      | +        | ESC ? k     | no
 *             , (comma)    | ,        | ESC ? l     | no
 *             - (minus)    | -        | ESC ? m     | no
 *             . (period)   | .        | ESC ? n     | yes
 *             / (divide)   | /        | ESC ? o     | no
 *             0            | 0        | ESC ? p     | yes
 *             1            | 1        | ESC ? q     | yes
 *             2            | 2        | ESC ? r     | yes
 *             3            | 3        | ESC ? s     | yes
 *             4            | 4        | ESC ? t     | yes
 *             5            | 5        | ESC ? u     | yes
 *             6            | 6        | ESC ? v     | yes
 *             7            | 7        | ESC ? w     | yes
 *             8            | 8        | ESC ? x     | yes
 *             9            | 9        | ESC ? y     | yes
 *             = (equal)    | =        | ESC ? X     | no
 *             -------------+----------+-------------+----------
 *
 *
 *
 * Sun-Style Function Keys
 *
 * The xterm program provides support for Sun keyboards more directly, by a
 * menu toggle that causes it to send Sun-style function key codes rather
 * than VT220.  Note, however, that the sun and VT100 emulations are not
 * really compatible.  For example, their wrap-margin behavior differs.
 *
 * Only function keys are altered; keypad and cursor keys are the same.
 * The emulation responds identically.  See the xterm-sun terminfo entry
 * for details.
 *
 *
 * HP-Style Function Keys
 *
 * Similarly, xterm can be compiled to support HP keyboards.  See the
 * xterm-hp terminfo entry for details.
 *
 *
 * The Alternate Screen Buffer
 *
 * XTerm maintains two screen buffers.  The Normal Screen Buffer allows you
 * to scroll back to view saved lines of output up to the maximum set by
 * the saveLines resource.  The Alternate Screen Buffer is exactly as large
 * as the display, contains no additional saved lines.  When the Alternate
 * Screen Buffer is active, you cannot scroll back to view saved lines.
 * XTerm provides control sequences and menu entries for switching between
 * the two.
 *
 * Most full-screen applications use terminfo or termcap to obtain strings
 * used to start/stop full-screen mode, i.e., smcup and rmcup for terminfo,
 * or the corresponding ti and te for termcap.  The titeInhibit resource
 * removes the ti and te strings from the TERMCAP string which is set in
 * the environment for some platforms.  That is not done when xterm is
 * built with terminfo libraries because terminfo does not provide the
 * whole text of the termcap data in one piece.  It would not work for ter-
 * minfo anyway, since terminfo data is not passed in environment vari-
 * ables; setting an environment variable in this manner would have no
 * effect on the application's ability to switch between Normal and Alter-
 * nate Screen buffers.  Instead, the newer private mode controls (such as
 * 1 0 4 9 ) for switching between Normal and Alternate Screen buffers sim-
 * ply disable the switching.  They add other features such as clearing the
 * display for the same reason: to make the details of switching indepen-
 * dent of the application that requests the switch.
 *
 *
 * Bracketed Paste Mode
 *
 * When bracketed paste mode is set, pasted text is bracketed with control
 * sequences so that the program can differentiate pasted text from typed-
 * in text.  When bracketed paste mode is set, the program will receive:
 *    ESC [ 2 0 0 ~ ,
 * followed by the pasted text, followed by
 *    ESC [ 2 0 1 ~ .
 *
 *
 * Title Modes
 *
 * The window- and icon-labels can be set or queried using control
 * sequences.  As a VT220-emulator, xterm "should" limit the character
 * encoding for the corresponding strings to ISO-8859-1.  Indeed, it used
 * to be the case (and was documented) that window titles had to be
 * ISO-8859-1.  This is no longer the case.  However, there are many appli-
 * cations which still assume that titles are set using ISO-8859-1.  So
 * that is the default behavior.
 *
 * If xterm is running with UTF-8 encoding, it is possible to use window-
 * and icon-labels encoded using UTF-8.  That is because the underlying X
 * libraries (and many, but not all) window managers support this feature.
 *
 * The utf8Title X resource setting tells xterm to disable a reconversion
 * of the title string back to ISO-8859-1, allowing the title strings to be
 * interpreted as UTF-8.  The same feature can be enabled using the title
 * mode control sequence described in this summary.
 *
 * Separate from the ability to set the titles, xterm provides the ability
 * to query the titles, returning them either in ISO-8859-1 or UTF-8.  This
 * choice is available only while xterm is using UTF-8 encoding.
 *
 * Finally, the characters sent to, or returned by a title control are less
 * constrained than the rest of the control sequences.  To make them more
 * manageable (and constrained), for use in shell scripts, xterm has an
 * optional feature which decodes the string from hexadecimal (for setting
 * titles) or for encoding the title into hexadecimal when querying the
 * value.
 *
 *
 * Mouse Tracking
 *
 * The VT widget can be set to send the mouse position and other informa-
 * tion on button presses.  These modes are typically used by editors and
 * other full-screen applications that want to make use of the mouse.
 *
 * There are two sets of mutually exclusive modes:
 *
 * o   mouse protocol
 *
 * o   protocol encoding
 *
 * The mouse protocols include DEC Locator mode, enabled by the DECELR CSI
 * Ps ; Ps '  z control sequence, and is not described here (control
 * sequences are summarized above).  The remaining five modes of the mouse
 * protocols are each enabled (or disabled) by a different parameter in the
 * "DECSET CSI ? Pm h " or "DECRST CSI ? Pm l " control sequence.
 *
 * Manifest constants for the parameter values are defined in xcharmouse.h
 * as follows:
 *
 *      #define SET_X10_MOUSE               9
 *      #define SET_VT200_MOUSE             1000
 *      #define SET_VT200_HIGHLIGHT_MOUSE   1001
 *      #define SET_BTN_EVENT_MOUSE         1002
 *      #define SET_ANY_EVENT_MOUSE         1003
 *
 *      #define SET_FOCUS_EVENT_MOUSE       1004
 *
 *      #define SET_EXT_MODE_MOUSE          1005
 *      #define SET_SGR_EXT_MODE_MOUSE      1006
 *      #define SET_URXVT_EXT_MODE_MOUSE    1015
 *
 *      #define SET_ALTERNATE_SCROLL        1007
 *
 * The motion reporting modes are strictly xterm extensions, and are not
 * part of any standard, though they are analogous to the DEC VT200 DECELR
 * locator reports.
 *
 * Normally, parameters (such as pointer position and button number) for
 * all mouse tracking escape sequences generated by xterm encode numeric
 * parameters in a single character as value+32.  For example, !  specifies
 * the value 1.  The upper left character position on the terminal is
 * denoted as 1,1.  This scheme dates back to X10, though the normal mouse-
 * tracking (from X11) is more elaborate.
 *
 *
 * X10 compatibility mode
 *
 * X10 compatibility mode sends an escape sequence only on button press,
 * encoding the location and the mouse button pressed.  It is enabled by
 * specifying parameter 9 to DECSET.  On button press, xterm sends CSI M
 * CbCxCy (6 characters).
 *
 * o   Cb is button-1, where button is 1, 2 or 3.
 *
 * o   Cx and Cy are the x and y coordinates of the mouse when the button
 *     was pressed.
 *
 *
 * Normal tracking mode
 *
 * Normal tracking mode sends an escape sequence on both button press and
 * release.  Modifier key (shift, ctrl, meta) information is also sent.  It
 * is enabled by specifying parameter 1000 to DECSET.  On button press or
 * release, xterm sends CSI M CbCxCy.
 *
 * o   The low two bits of Cb encode button information: 0=MB1 pressed,
 *     1=MB2 pressed, 2=MB3 pressed, 3=release.
 *
 * o   The next three bits encode the modifiers which were down when the
 *     button was pressed and are added together:  4=Shift, 8=Meta, 16=Con-
 *     trol.  Note however that the shift and control bits are normally
 *     unavailable because xterm uses the control modifier with mouse for
 *     popup menus, and the shift modifier is used in the default transla-
 *     tions for button events.  The Meta modifier recognized by xterm is
 *     the mod1 mask, and is not necessarily the "Meta" key (see
 *     xmodmap(1)).
 *
 * o   Cx and Cy are the x and y coordinates of the mouse event, encoded as
 *     in X10 mode.
 *
 *
 * Wheel mice
 *
 * Wheel mice may return buttons 4 and 5.  Those buttons are represented by
 * the same event codes as buttons 1 and 2 respectively, except that 64 is
 * added to the event code.  Release events for the wheel buttons are not
 * reported.
 *
 * By default, the wheel mouse events (buttons 4 and 5) are translated to
 * scroll-back and scroll-forw actions, respectively.  Those actions nor-
 * mally scroll the whole window, as if the scrollbar was used.
 *
 * However if Alternate Scroll mode is set, then cursor up/down controls
 * are sent when the terminal is displaying the Alternate Screen Buffer.
 * The initial state of Alternate Scroll mode is set using the alternate-
 * Scroll resource.
 *
 *
 * Other buttons
 *
 * Some wheel mice can send additional button events, e.g., by tilting the
 * scroll wheel left and right.
 *
 * Additional buttons are encoded like the wheel mice,
 *
 * o   by adding 64 (for buttons 6 and 7), or
 *
 * o   by adding 128 (for buttons 8 through 11).
 *
 * Past button 11, the encoding is ambiguous because the same code may cor-
 * respond to different button/modifier combinations.
 *
 * It is not possible to use these buttons (6-11) in xterm's translations
 * resource because their names are not in the X Toolkit's symbol table.
 * However, applications can check for the reports, e.g., button 7 (left)
 * and button 6 (right) with a Logitech mouse.
 *
 *
 * Highlight tracking
 *
 * Mouse highlight tracking notifies a program of a button press, receives
 * a range of lines from the program, highlights the region covered by the
 * mouse within that range until button release, and then sends the program
 * the release coordinates.  It is enabled by specifying parameter 1001 to
 * DECSET.  Highlighting is performed only for button 1, though other but-
 * ton events can be received.
 *
 * Warning: this mode requires a cooperating program, else xterm will hang.
 *
 * On button press, the same information as for normal tracking is gener-
 * ated; xterm then waits for the program to send mouse tracking informa-
 * tion.  All X events are ignored until the proper escape sequence is
 * received from the pty:
 * CSI Ps ; Ps ; Ps ; Ps ; Ps T
 *
 * The parameters are func, startx, starty, firstrow, and lastrow:
 *
 * o   func is non-zero to initiate highlight tracking and zero to abort.
 *
 * o   startx and starty give the starting x and y location for the high-
 *     lighted region.
 *
 * o   The ending location tracks the mouse, but will never be above row
 *     firstrow and will always be above row lastrow.  (The top of the
 *     screen is row 1.)
 *
 * When the button is released, xterm reports the ending position one of
 * two ways:
 *
 * o   if the start and end coordinates are the same locations:
 *
 *     CSI t CxCy
 *
 * o   otherwise:
 *
 *     CSI T CxCyCxCyCxCy
 *
 * The parameters are startx, starty, endx, endy, mousex, and mousey:
 *
 * o   startx, starty, endx, and endy give the starting and ending charac-
 *     ter positions of the region.
 *
 * o   mousex and mousey give the location of the mouse at button up, which
 *     may not be over a character.
 *
 *
 * Button-event tracking
 *
 * Button-event tracking is essentially the same as normal tracking, but
 * xterm also reports button-motion events.  Motion events are reported
 * only if the mouse pointer has moved to a different character cell.  It
 * is enabled by specifying parameter 1002 to DECSET.  On button press or
 * release, xterm sends the same codes used by normal tracking mode.
 *
 * o   On button-motion events, xterm adds 32 to the event code (the third
 *     character, Cb).
 *
 * o   The other bits of the event code specify button and modifier keys as
 *     in normal mode.  For example, motion into cell x,y with button 1
 *     down is reported as
 *
 *     CSI M @ CxCy
 *
 *     ( @  = 32 + 0 (button 1) + 32 (motion indicator) ).  Similarly,
 *     motion with button 3 down is reported as
 *
 *     CSI M B CxCy
 *
 *     ( B  = 32 + 2 (button 3) + 32 (motion indicator) ).
 *
 *
 * Any-event tracking
 *
 * Any-event mode is the same as button-event mode, except that all motion
 * events are reported, even if no mouse button is down.  It is enabled by
 * specifying 1003 to DECSET.
 *
 *
 * FocusIn/FocusOut
 *
 * FocusIn/FocusOut can be combined with any of the mouse events since it
 * uses a different protocol.  When set, it causes xterm to send CSI I
 * when the terminal gains focus, and CSI O  when it loses focus.
 *
 *
 * Extended coordinates
 *
 * The original X10 mouse protocol limits the Cx and Cy ordinates to 223
 * (=255 - 32).  XTerm supports more than one scheme for extending this
 * range, by changing the protocol encoding:
 *
 * UTF-8 (1005)
 *           This enables UTF-8 encoding for Cx and Cy under all tracking
 *           modes, expanding the maximum encodable position from 223 to
 *           2015.  For positions less than 95, the resulting output is
 *           identical under both modes.  Under extended mouse mode, posi-
 *           tions greater than 95 generate "extra" bytes which will con-
 *           fuse applications which do not treat their input as a UTF-8
 *           stream.  Likewise, Cb will be UTF-8 encoded, to reduce confu-
 *           sion with wheel mouse events.
 *
 *           Under normal mouse mode, positions outside (160,94) result in
 *           byte pairs which can be interpreted as a single UTF-8 charac-
 *           ter; applications which do treat their input as UTF-8 will
 *           almost certainly be confused unless extended mouse mode is
 *           active.
 *
 *           This scheme has the drawback that the encoded coordinates will
 *           not pass through luit(1) unchanged, e.g., for locales using
 *           non-UTF-8 encoding.
 *
 * SGR (1006)
 *           The normal mouse response is altered to use
 *
 *           o   CSI < followed by semicolon-separated
 *
 *           o   encoded button value,
 *
 *           o   Px and Py ordinates and
 *
 *           o   a final character which is M  for button press and m  for
 *               button release.
 *
 *           The encoded button value in this case does not add 32 since
 *           that was useful only in the X10 scheme for ensuring that the
 *           byte containing the button value is a printable code.
 *
 *           o   The modifiers are encoded in the same way.
 *
 *           o   A different final character is used for button release to
 *               resolve the X10 ambiguity regarding which button was
 *               released.
 *
 *           The highlight tracking responses are also modified to an SGR-
 *           like format, using the same SGR-style scheme and button-encod-
 *           ings.
 *
 * URXVT (1015)
 *           The normal mouse response is altered to use
 *
 *           o   CSI followed by semicolon-separated
 *
 *           o   encoded button value,
 *
 *           o   the Px and Py ordinates and final character M .
 *
 *           This uses the same button encoding as X10, but printing it as
 *           a decimal integer rather than as a single byte.
 *
 *           However, CSI M  can be mistaken for DL (delete lines), while
 *           the highlight tracking CSI T  can be mistaken for SD (scroll
 *           down), and the Window manipulation controls.  For these rea-
 *           sons, the 1015 control is not recommended; it is not an
 *           improvement over 1006.
 *
 *
 * Sixel Graphics
 *
 * If xterm is configured as VT240, VT241, VT330, VT340 or VT382 using the
 * decTerminalID resource, it supports Sixel Graphics controls, a palleted
 * bitmap graphics system using sets of six vertical pixels as the basic
 * element.
 *
 * CSI Ps c  Send Device Attributes (Primary DA), xterm.  xterm responds to
 *           Send Device Attributes (Primary DA) with these additional
 *           codes:
 *             Ps = 4  -> Sixel graphics.
 *
 * CSI ? Pm h
 *           Set Mode, xterm.  xterm has these additional private Set Mode
 *           values:
 *             Ps = 8 0  -> Sixel scrolling.
 *             Ps = 1 0 7 0  -> use private color registers for each
 *           graphic.
 *             Ps = 8 4 5 2  -> Sixel scrolling leaves cursor to right of
 *           graphic.
 *
 * DCS Pa ; Pb ; Ph q  Ps..Ps ST
 *           Send SIXEL image, DEC graphics terminals, xterm.  See:
 *
 *              VT330/VT340 Programmer Reference Manual Volume 2:
 *              Graphics Programming
 *              Chapter 14 Graphics Programming
 *
 *           The sixel data device control string has three positional
 *           parameters, following the q  with sixel data.
 *             Pa -> pixel aspect ratio
 *             Pb -> background color option
 *             Ph -> horizontal grid size (ignored).
 *             Ps -> sixel data
 *
 *
 * ReGIS Graphics
 *
 * If xterm is configured as VT125, VT240, VT241, VT330 or VT340 using the
 * decTerminalID resource, it supports Remote Graphic Instruction Set, a
 * graphics description language.
 *
 * CSI Ps c  Send Device Attributes (Primary DA), DEC graphics terminals,
 *           xterm.  xterm responds to Send Device Attributes (Primary DA)
 *           with these additional codes:
 *             Ps = 3  -> ReGIS graphics.
 *
 * CSI ? Pm h
 *           Set Mode, xterm.  xterm has these additional private Set Mode
 *           values:
 *             Ps = 1 0 7 0  -> use private color registers for each
 *           graphic.
 *
 * DCS Pm p Pr..Pr ST
 *           Enter or exit ReGIS, VT300, xterm.  See:
 *
 *              VT330/VT340 Programmer Reference Manual Volume 2:
 *              Graphics Programming
 *              Chapter 1 Introduction to ReGIS
 *
 *           The ReGIS data device control string has one positional param-
 *           eter with four possible values:
 *             Pm = 0 -> resume command, use fullscreen mode.
 *             Pm = 1 -> start new command, use fullscreen mode.
 *             Pm = 2 -> resume command, use command display mode.
 *             Pm = 3 -> start new command, use command display mode.
 *
 *
 * Tektronix 4014 Mode
 *
 * Most of these sequences are standard Tektronix 4014 control sequences.
 * Graph mode supports the 12-bit addressing of the Tektronix 4014.  The
 * major features missing are the write-through and defocused modes.  This
 * document does not describe the commands used in the various Tektronix
 * plotting modes but does describe the commands to switch modes.
 *
 * Some of the sequences are specific to xterm.  The Tektronix emulation
 * was added in X10R4 (1986).  The VT240, introduced two years earlier,
 * also supported Tektronix 4010/4014.  Unlike xterm, the VT240 documenta-
 * tion implies (there is an obvious error in section 6.9 "Entering and
 * Exiting 4010/4014 Mode") that exiting back to ANSI mode is done by
 * resetting private mode 3 8  (DECTEK) rather than ESC ETX .  A real Tek-
 * tronix 4014 would not respond to either.
 *
 * BEL       Bell (Ctrl-G).
 *
 * BS        Backspace (Ctrl-H).
 *
 * TAB       Horizontal Tab (Ctrl-I).
 *
 * LF        Line Feed or New Line (Ctrl-J).
 *
 * VT        Cursor up (Ctrl-K).
 *
 * FF        Form Feed or New Page (Ctrl-L).
 *
 * CR        Carriage Return (Ctrl-M).
 *
 * ESC ETX   Switch to VT100 Mode (ESC  Ctrl-C).
 *
 * ESC ENQ   Return Terminal Status (ESC  Ctrl-E).
 *
 * ESC FF    PAGE (Clear Screen) (ESC  Ctrl-L).
 *
 * ESC SO    Begin 4015 APL mode (ESC  Ctrl-N).  This is ignored by xterm.
 *
 * ESC SI    End 4015 APL mode (ESC  Ctrl-O).  This is ignored by xterm.
 *
 * ESC ETB   COPY (Save Tektronix Codes to file COPYyyyy-mm-dd.hh:mm:ss).
 *             ETB  (end transmission block) is the same as Ctrl-W.
 *
 * ESC CAN   Bypass Condition (ESC  Ctrl-X).
 *
 * ESC SUB   GIN mode (ESC  Ctrl-Z).
 *
 * ESC FS    Special Point Plot Mode (ESC  Ctrl-\).
 *
 * ESC 8     Select Large Character Set.
 *
 * ESC 9     Select #2 Character Set.
 *
 * ESC :     Select #3 Character Set.
 *
 * ESC ;     Select Small Character Set.
 *
 * OSC Ps ; Pt BEL
 *           Set Text Parameters of VT window.
 *             Ps = 0  -> Change Icon Name and Window Title to Pt.
 *             Ps = 1  -> Change Icon Name to Pt.
 *             Ps = 2  -> Change Window Title to Pt.
 *             Ps = 4 6  -> Change Log File to Pt.  This is normally dis-
 *           abled by a compile-time option.
 *
 * ESC `     Normal Z Axis and Normal (solid) Vectors.
 *
 * ESC a     Normal Z Axis and Dotted Line Vectors.
 *
 * ESC b     Normal Z Axis and Dot-Dashed Vectors.
 *
 * ESC c     Normal Z Axis and Short-Dashed Vectors.
 *
 * ESC d     Normal Z Axis and Long-Dashed Vectors.
 *
 * ESC h     Defocused Z Axis and Normal (solid) Vectors.
 *
 * ESC i     Defocused Z Axis and Dotted Line Vectors.
 *
 * ESC j     Defocused Z Axis and Dot-Dashed Vectors.
 *
 * ESC k     Defocused Z Axis and Short-Dashed Vectors.
 *
 * ESC l     Defocused Z Axis and Long-Dashed Vectors.
 *
 * ESC p     Write-Thru Mode and Normal (solid) Vectors.
 *
 * ESC q     Write-Thru Mode and Dotted Line Vectors.
 *
 * ESC r     Write-Thru Mode and Dot-Dashed Vectors.
 *
 * ESC s     Write-Thru Mode and Short-Dashed Vectors.
 *
 * ESC t     Write-Thru Mode and Long-Dashed Vectors.
 *
 * FS        Point Plot Mode (Ctrl-\).
 *
 * GS        Graph Mode (Ctrl-]).
 *
 * RS        Incremental Plot Mode (Ctrl-^ ).
 *
 * US        Alpha Mode (Ctrl-_).
 *
 *
 * VT52 Mode
 *
 * Parameters for cursor movement are at the end of the ESC Y  escape
 * sequence.  Each ordinate is encoded in a single character as value+32.
 * For example, !  is 1.  The screen coordinate system is 0-based.
 *
 * ESC <     Exit VT52 mode (Enter VT100 mode).
 *
 * ESC =     Enter alternate keypad mode.
 *
 * ESC >     Exit alternate keypad mode.
 *
 * ESC A     Cursor up.
 *
 * ESC B     Cursor down.
 *
 * ESC C     Cursor right.
 *
 * ESC D     Cursor left.
 *
 * ESC F     Enter graphics mode.
 *
 * ESC G     Exit graphics mode.
 *
 * ESC H     Move the cursor to the home position.
 *
 * ESC I     Reverse line feed.
 *
 * ESC J     Erase from the cursor to the end of the screen.
 *
 * ESC K     Erase from the cursor to the end of the line.
 *
 * ESC Y Ps Ps
 *           Move the cursor to given row and column.
 *
 * ESC Z     Identify.
 *             -> ESC  /  Z  ("I am a VT52.").
 *
 *
 * Further reading
 *
 *
 * Technical manuals
 *
 * Manuals for hardware terminals are more readily available than simi-
 * larly-detailed documentation for terminal emulators such as aixterm,
 * shelltool, dtterm.
 *
 * However long, the technical manuals have problems:
 *
 * o   DEC's manuals did not provide a comprehensive comparison of the fea-
 *     tures in different model.
 *
 *     Peter Sichel's Host Interface Functions Checklist spreadsheet is
 *     useful for noting which model introduced a given feature (although
 *     there are a few apparent errors such as the DECRQSS feature cited
 *     for VT320 whereas the technical manual omits it).
 *
 * o   Sometimes the manuals disagree.  For example, DEC's standard docu-
 *     ment (DEC STD 070) for terminals says that DECSCL performs a soft
 *     reset (DECSTR), while the VT420 manual says it does a hard reset
 *     (RIS).
 *
 * o   Sometimes the manuals are simply incorrect.  For example, testing a
 *     DEC VT420 in 1996 showed that the documented code for a valid or
 *     invalid response to DECRQSS was reversed.
 *
 *     The VT420 test results were incorporated into vttest program.  At
 *     the time, DEC STD 070 was not available, but it also agrees with
 *     vttest.  Later, documentation for the DEC VT525 was shown to have
 *     the same flaw.
 *
 * o   Not all details are clear even in DEC STD 070 (which is more than
 *     twice the length of the VT520 programmer's reference manual, and
 *     almost three times longer than the VT420 reference manual).  How-
 *     ever, as an internal standards document, DEC STD 070 is more likely
 *     to describe the actual behavior of DEC's terminals than the more
 *     polished user's guides.
 *
 * That said, here are technical manuals which have been used in developing
 * xterm.  Not all were available initially.  In August 1996 for instance,
 * the technical references were limited to EK-VT220-HR-002 and EK-
 * VT420-UG.002.  Shortly after, Richard Shuford sent a copy of EK-VT3XX-
 * TP-001.  Still later (beginning in 2003), Paul Williams' vt100.net site
 * provided EK-VT102-UG-003, EK-VT220-RM-002, EK-VT420-RM-002, EK-VT520-RM
 * A01, EK-VT100-TM-003, and EK-VT102-UG-003.  The remaining documents were
 * found on the bitsavers site.
 *
 * o   DECscope User's Manual.
 *     Digital Equipment Corporation (EK-VT5X-OP-001 1975).
 *
 * o   VT100 Series Video Terminal Technical Manual.
 *     Digital Equipment Corporation (EK-VT100-TM-003, July 1982).
 *
 * o   VT100 User Guide.
 *     Digital Equipment Corporation (EK-VT100-UG-003, June 1981).
 *
 * o   VT102 User Guide.
 *     Digital Equipment Corporation (EK-VT102-UG-003, February 1982).
 *
 * o   VT220 Programmer Pocket Guide.
 *     Digital Equipment Corporation (EK-VT220-HR-002, July 1984).
 *
 * o   VT220 Programmer Reference Manual.
 *     Digital Equipment Corporation (EK-VT220-RM-002, August 1984).
 *
 * o   VT240 Programmer Reference Manual.
 *     Digital Equipment Corporation (EK-VT240-RM-002, October 1984).
 *
 * o   VT330/VT340 Programmer Reference Manual
 *     Volume 1: Text Programming.
 *     Digital Equipment Corporation (EK-VT3XX-TP-001, March 1987).
 *
 * o   VT330/VT340 Programmer Reference Manual
 *     Volume 2: Graphics Programming.
 *     Digital Equipment Corporation (EK-VT3XX-GP-001, March 1987).
 *
 * o   Installing and Using
 *     The VT420 Video Terminal
 *     (North American Model).
 *     Digital Equipment Corporation (EK-VT420-UG.002, February 1990).
 *
 * o   VT420 Programmer Reference Manual.
 *     Digital Equipment Corporation (EK-VT420-RM-002, February 1992).
 *
 * o   VT510 Video Terminal
 *     Programmer Information.
 *     Digital Equipment Corporation (EK-VT510-RM B01, November 1993).
 *
 * o   VT520/VT525 Video Terminal
 *     Programmer Information.
 *     Digital Equipment Corporation (EK-VT520-RM A01, July 1994).
 *
 * o   Digital ANSI-Compliant Printing Protocol
 *     Level 2 Programming Reference Manual
 *     Digital Equipment Corporation (EK-PPLV2-PM B01, August 1994).
 *
 * o   4014 and 4014-1 Computer Display Terminal
 *     User's Manual.
 *     Tektronix, Inc.  (070-1647-00, November 1979).
 *
 *
 * Standards
 *
 * The DEC terminal family (VT100 through VT525) is upward-compatible,
 * using standards plus extensions, e.g., "private modes".  Not all com-
 * monly-used features are standard.  For example, scrolling regions are
 * not found in ECMA-48.
 *
 * Again, it is possible to find discrepancies in the standards:
 *
 * o   The printed ECMA-48 5th edition (1991) and the first PDF produced
 *     for that edition (April 1998) state that SD (scroll down) ends with
 *     05/14, i.e., ^ , which disagrees with DEC's VT420 hardware implemen-
 *     tation and DEC's manuals which use 05/04 T .  (A few other terminals
 *     such as AT&T 5620 and IBM 5151 also used 05/04, but the documenta-
 *     tion and dates are lacking).
 *
 *     ECMA created a new PDF in April 2003 which changed that detail to
 *     use T , and later in 2008 provided PDFs of the earlier editions
 *     which used T .
 *
 * o   The VT320, VT420, VT520 manuals claim that DECSCL does a hard reset
 *     (RIS).
 *
 *     Both the VT220 manual and DEC STD 070 (which documents levels 1-4 in
 *     detail) state that it is a soft reset, e.g., DECSTR.
 *
 * Here are the relevant standards:
 *
 * o   ECMA-35: Character Code Structure and Extension Techniques
 *     (6th Edition, December 1994).
 *
 * o   ECMA-43: 8-bit Coded Character Set Structure and Rules
 *     (3rd Edition, December 1991).
 *
 * o   ECMA-48: Control Functions for Coded Character Sets
 *     (5th Edition, June 1991).
 *
 * o   DEC STD 070 Video Systems Reference Manual.
 *     Digital Equipment Corporation (A-MN-ELSM070-00-0000 Rev H, December
 *     3, 1991).
 *
 *
 * Miscellaneous
 *
 * A few hardware terminals survived into the 1990s only as terminal emula-
 * tors.  Documentation for these and other terminal emulators which have
 * influenced xterm are generally available only in less-accessible and
 * less-detailed manual pages.
 *
 * o   XTerm supports control sequences for manipulating its window which
 *     were implemented by Sun's shelltool program.  This was part of Sun-
 *     View (SunOS 3.0, 1986).  The change-notes for xterm's resize program
 *     in X10.4 (1986) mention its use of these "Sun tty emulation escape
 *     sequences" for resizing the window.  The X10.4 xterm program recog-
 *     nized these sequences for resizing the terminal, except for the
 *     iconfig/deiconfy pair.  SunView also introduced the SIGWINCH signal,
 *     used by the X10.4 xterm and mentioned in its CHANGES file:
 *
 *         The window size is passed to the operating system via TIOCSWINSZ
 *         (4.3) or TIOCSSIZE (sun).  A SIGWINCH signal is sent if the
 *         vtXXX window is resized.
 *
 *     While support for the Sun control-sequences remained in resize, the
 *     next release of xterm (X11R1 in 1987) omitted the code for inter-
 *     preting them.
 *
 *     Later, the SunView program was adapted for the OPEN LOOK environment
 *     introduced 1988-1990.
 *
 *     Still later, in 1995, OPEN LOOK was abandoned in favor of CDE.  The
 *     CDE terminal emulator dtterm implemented those controls, with a cou-
 *     ple of additions.
 *
 *     Starting in July 1996, xterm re-implemented those control sequences
 *     (based on the dtterm manual pages) and further extended the group of
 *     window controls.
 *
 *     There were two sets of controls (CSI Ps[ ; Pm ; Pm]t , and OSC
 *     PstextST ) implemented by shelltool, documented in appendix E of
 *     both PHIGS Programming Manual (1992), and the unpublished X Window
 *     System User's Guide (OPEN LOOK Edition) (1995).  The CDE program
 *     kept those, and added a few new ones.
 *
 *     Code         Sun   CDE   XTerm   Description
 *     -----------------------------------------------------------------
 *     CSI 1 t      yes   yes    yes    de-iconify
 *     CSI 2 t      yes   yes    yes    iconify
 *     CSI 3 t      yes   yes    yes    move window to pixel-position
 *     CSI 4 t      yes   yes    yes    resize window in pixels
 *     CSI 5 t      yes   yes    yes    raise window to front of stack
 *     CSI 6 t      yes   yes    yes    raise window to back of stack
 *     CSI 7 t      yes   yes    yes    refresh window
 *     CSI 8 t      yes   yes    yes    resize window in chars
 *     CSI 9 t       -     -     yes    maximize/unmaximize window
 *     CSI 1 0 t     -     -     yes    to/from full-screen
 *     CSI 1 1 t    yes   yes    yes    report if window is iconified
 *     CSI 1 2 t     -     -      -     -
 *     CSI 1 3 t    yes   yes    yes    report window position
 *     CSI 1 4 t    yes   yes    yes    report window size in pixels
 *     CSI 1 5 t     -     -     yes    report screen size in pixels
 *     CSI 1 6 t     -     -     yes    report character cell in pixels
 *     CSI 1 7 t     -     -      -     -
 *     CSI 1 8 t    yes   yes    yes    report window size in chars
 *     CSI 1 9 t     -     -     yes    report screen size in chars
 *     CSI 2 0 t     -    yes    yes    report icon label
 *     CSI 2 1 t     -    yes    yes    report window title
 *     CSI 2 2 t     -     -     yes    save window/icon title
 *     CSI 2 3 t     -     -     yes    restore window/icon title
 *     CSI 2 4 t     -     -     yes    resize window (DECSLPP)
 *     OSC 0 ST      -    yes    yes    set window and icon title
 *     OSC 1 ST      -    yes    yes    set icon label
 *     OSC 2 ST      -    yes    yes    set window title
 *     OSC 3 ST      -    n/a    yes    set X server property
 *     OSC I ST     yes   yes    yes    set icon to file
 *     OSC l ST     yes   yes    yes    set window title
 *     OSC L ST     yes   yes    yes    set icon label
 *
 *     Besides the Sun-derived OSC controls for setting window title and
 *     icon label, dtterm also supported the xterm controls for the same
 *     feature.
 *
 *     The CDE source was unavailable for inspection until 2012, so that
 *     clarification of the details of the window operations relied upon
 *     vttest.
 *
 * o   The control sequences for saving/restoring the cursor and for sav-
 *     ing/restoring "DEC Private Mode Values" may appear to be related
 *     (since the "save" controls both end with s ), but that is coinciden-
 *     tal.  The latter was introduced in X10.4:
 *
 *         Most Dec Private mode settings can be save away internally using
 *         \E[?ns, where n is the same number to set or reset the Dec
 *         Private mode.  The mode can be restored using \E[?nr.  This can
 *         be used in termcap for vi, for example, to turn off saving of
 *         lines, but restore whatever the original state was on exit.
 *
 *     while  the  SCOSC/SCORC pair was added in 1995 by XFree86 (and docu-
 *     mented long afterwards).
 *
 * o   The aixterm manual page gives the format of the control sequence for
 *     foreground  and  background  colors  8-15, but does not specify what
 *     those colors are.  That is implied by the description's  mention  of
 *     HFT:
 *
 *         The aixterm command provides a standard terminal type for
 *         programs that do not interact directly with Enhanced X-Windows.
 *         This command provides an emulation for a VT102 terminal or a
 *         high function terminal (HFT).  The VT102 mode is activated by
 *         the -v flag.
 *
 *     Unlike xterm, there are no resource names for the 16 colors, leaving
 *     the reader to assume that the mapping is  hard-coded.   The  control
 *     sequences  for  colors 8-15 are not specified by ECMA-48, but rather
 *     (as done in other instances by xterm) chosen to  not  conflict  with
 *     current or future standards.
 *  * </pre>
 */
