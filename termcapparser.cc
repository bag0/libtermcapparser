#include "puttyparser.hh"

#include "terminal.h"

#include "putty/exception.hh"
#include "putty/termcapparser.hh"
#include <sstream>

using namespace Putty;

namespace
{

#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)
  /**
   * Determine if a character is a unicode combining character.
   *
   * For information about combining characters, see:
   *
   *   http://en.wikipedia.org/wiki/Combining_character
   *
   * For the unicode character sheets, see:
   *
   *   Combining Diacritical Marks: http://www.unicode.org/charts/PDF/U0300.pdf
   *   Combining Diacritical Marks Supplement: http://www.unicode.org/charts/PDF/U1DC0.pdf
   *   Combining Diacritical Marks for Symbols: http://www.unicode.org/charts/PDF/U20D0.pdf
   *   Combining Half Marks: http://www.unicode.org/charts/PDF/UFE20.pdf
   *
   */
  bool is_combining_character(wchar_t chr)
  {
    if (likely(chr < 0x0300))
      return false;

    // Diacritical marks
    if (chr >= 0x0300 && chr <= 0x036F)
      return true;

    // Diacritical marks supplement - range 1
    if (chr >= 0x1DC0 && chr <= 0x1DE6)
      return true;

    // Diacritical marks supplement - range 2
    if (chr >= 0x1DFC && chr <= 0x1DFF)
      return true;

    // Diacritical marks for symbols
    if (chr >= 0x20D0 && chr <= 0x20F0)
      return true;

    // Half marks
    if (chr >= 0xFE20 && chr <= 0xFE26)
      return true;

    return false;
  }

  /**
   * Helper class for changing the value of a variable temporarily (just for the given scope)
   * in an exception-safe manner. When the destructor is called (when exiting the scope), the
   * value is re-set to its old value.
   */
  template < typename _Type >
    class TemporalValueChange
    {
    public:
      /**
       * Initialize helper object. The reference value is changed to the new value.
       *
       * @param ref Reference to variable to change.
       * @param value New value of the variable.
       */
      TemporalValueChange(_Type &ref, const _Type &value)
        : ref(ref),
          oldval(ref)
      {
        ref = value;
      }

      /**
       * Destroy the helper object. The referenced variable is set to its old value.
       */
      ~TemporalValueChange()
      {
        ref = oldval;
      }

    private:
      _Type &ref;     /**< Variable reference */
      _Type oldval;   /**< Old value of the variable (before change) */
    };
}

TermcapParser::TermcapParser(char *charset)
  : enable_update_display(true)
{
  /* Create an instance structure and initialise to zeroes */
  inst = snew(struct gui_data);
  memset(inst, 0, sizeof(*inst));

  /* Set the termcap parser object */
  inst->parser = this;

  /* FIXME: decide how to set the terminals initial size */
  const int initial_terminal_width = 80;
  const int initial_terminal_height = 24;
  const int initial_buffer_height = 100000;

  /* set instance dimensions */
  set_buffer_size(initial_terminal_width, initial_buffer_height);

  /* initialize unicode config */
  init_ucs(&inst->ucsdata, charset, 0, CS_UTF8, VT_UNICODE);

  /* Enable ANSI and XTerm 256 colors */
  inst->cfg.ansi_colour = 1;
  inst->cfg.xterm_256_colour = 1;

  /* Configure BiDirectional and Arabic text support, 0 means enable and 1 means disable */
  inst->cfg.bidi = 1;
  inst->cfg.arabicshaping = 1;

  /* DO NOT REMOVE! This config option ensures that an erase will be done with the proper bg color. */
  inst->cfg.bce = 1;

  /* Set the buffer lines to save */
  inst->cfg.savelines = initial_buffer_height - initial_terminal_height;

  /*
   * LF implies a CR-LF since in some cases (like ssh command execution) only LF is sent.
   * By default, we don't enable this but replace_standalone_linefeeds() can be used to
   * change this behaviour.
   */
  inst->cfg.lfhascr = 0;

  /* initialize terminal structure */
  inst->term = term_init(&inst->cfg, &inst->ucsdata, inst);
  inst->term->ldisc = 0;

  /* Set log context */
  inst->logctx = NULL;
  term_provide_logctx(inst->term, inst->logctx);

  /* set terminal visual size: row number, column number, saveline option */
  set_terminal_size(initial_terminal_width, initial_terminal_height);
}

TermcapParser::~TermcapParser()
{
  term_free(inst->term);
  sfree(inst);
}

void
TermcapParser::data_input(const char *data, int len)
{
  int prev = 0;

  for (int pos = 0; pos != len; ++pos)
    {
      if (data[pos] == '\033' && (pos != len - 1 && data[pos + 1] == 'P'))
        {
          data_input_filtered(data + prev, pos - prev);
          pos += 7;
          prev = pos + 1;
        }
    }

  data_input_filtered(data + prev, len - prev);
}

void
TermcapParser::data_input_filtered(const char *data, int len)
{
  /* inject input in the terminal */
  term_data(inst->term, 0, data, len);
}

void
TermcapParser::set_buffer_size(int width, int height)
{
  inst->width = width;
  inst->height = height;
}

void
TermcapParser::set_terminal_size(int width, int height)
{
  state.resize_display(width, height);
  term_size(inst->term, height, width, inst->cfg.savelines);
  set_buffer_size(inst->height, width);
}

void
TermcapParser::copy_term_content_to_cache(int offset, unsigned row_count) const
{
  term_scroll(inst->term, -1, offset);

  std::wstring characters;
  int absolute_offset = 0;
  for (unsigned row = 0; row < row_count; row++)
    {
      state.get_row_internal(offset + (int)row).set_attributes(inst->term->disptext[row]->lattr);
      for (unsigned col = 0; col < (unsigned)inst->term->cols; col++)
        {
          int relative_offset;
          absolute_offset = col;

          // load the character and all its combining characters
          do
            {
              characters.push_back( inst->term->disptext[row]->chars[ absolute_offset ].chr );
              relative_offset = inst->term->disptext[row]->chars[ absolute_offset ].cc_next;
              absolute_offset += relative_offset;
            } while(relative_offset != 0);
          state.set_cell(offset + (int)row, col,
                           characters,
                           inst->term->disptext[row]->chars[col].attr);
          characters.clear();
        }
    }
}

const State &
TermcapParser::get_state() const
{
  int buffer_line_count = sblines(inst->term);
  state.resize(inst->term->cols, inst->term->rows, buffer_line_count);
  state.set_palette(palette);

  /* update terminal display buffer */
  term_update(inst->term);

  {
    /* write buffer content */
    int offset;
    for (offset = -buffer_line_count; offset <= -inst->term->rows; offset += inst->term->rows)
      {
        copy_term_content_to_cache(offset, inst->term->rows);
      }
    /* write remainder buffer content which is less than a complete terminal screen */
    copy_term_content_to_cache(offset, -offset);
  }

  /*
   * Scroll to the current content of the display. The content will be updated
   * automatically by update_display.
   */
  term_scroll(inst->term, -1, 0);

  return state;
}

void
TermcapParser::replace_standalone_linefeeds(bool enable)
{
  inst->term->cfg.lfhascr = (enable ? 1 : 0);
}

void
TermcapParser::clear_buffer()
{
  term_clrsb(inst->term);
}

void
TermcapParser::update_display(int x, int y, const std::wstring &str, unsigned long attr, long lattr)
{
  if (!enable_update_display)
    return;

  std::wstring chr;
  state.get_row_internal(y).set_attributes(lattr);
  for (std::wstring::const_iterator it = str.begin(); it != str.end(); ++it)
    {
      if (it != str.begin() && !is_combining_character(*it))
        {
          state.set_cell(y, x, chr, attr);
          ++x;
          chr.clear();

        }
      chr.push_back(*it);
    }

  if (!chr.empty())
    state.set_cell(y, x, chr, attr);
}