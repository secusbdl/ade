#include "TerminalModel.h"
#include <QUrl>
#include <Qt>
#include <cmath>
#include <cstring>

namespace ade {
namespace {
VTermModifier modifiers(Qt::KeyboardModifiers value){return VTermModifier((value.testFlag(Qt::ShiftModifier)?VTERM_MOD_SHIFT:0)|(value.testFlag(Qt::AltModifier)?VTERM_MOD_ALT:0)|(value.testFlag(Qt::ControlModifier)?VTERM_MOD_CTRL:0));}
QColor rgb(VTermState *state,VTermColor color){vterm_state_convert_color_to_rgb(state,&color);return QColor(color.rgb.red,color.rgb.green,color.rgb.blue);}
VTermColor vtColor(QColor color){VTermColor result;vterm_color_rgb(&result,color.red(),color.green(),color.blue());return result;}
QColor oklch(double light,double chroma,double hue){
    const double radians=hue*3.14159265358979323846/180.,a=chroma*std::cos(radians),b=chroma*std::sin(radians);
    const double l=std::pow(light+.3963377774*a+.2158037573*b,3),m=std::pow(light-.1055613458*a-.0638541728*b,3),s=std::pow(light-.0894841775*a-1.291485548*b,3);
    auto encode=[](double v){return qBound(0.,v<=.0031308?12.92*v:1.055*std::pow(v,1/2.4)-.055,1.);};
    return QColor::fromRgbF(encode(4.0767416621*l-3.3077115913*m+.2309699292*s),encode(-1.2684380046*l+2.6097574011*m-.3413193965*s),encode(-.0041960863*l-.7034186147*m+1.707614701*s));
}
}
TerminalModel::TerminalModel(QSize cells,QObject *parent):QObject(parent),m_size(cells.expandedTo({2,1}).boundedTo({500,300})){
    m_terminal=vterm_new(m_size.height(),m_size.width());vterm_set_utf8(m_terminal,1);
    m_state=vterm_obtain_state(m_terminal);m_screen=vterm_obtain_screen(m_terminal);
    static const VTermScreenCallbacks callbacks={
        [](VTermRect,void*){return 1;},nullptr,[](VTermPos,VTermPos,int,void*){return 1;},property,
        [](void *p){emit static_cast<TerminalModel*>(p)->bell();return 1;},nullptr,pushLine,popLine,clearHistory};
    static const VTermStateFallbacks fallbacks={nullptr,nullptr,osc,nullptr,nullptr,nullptr,nullptr};
    vterm_screen_set_callbacks(m_screen,&callbacks,this);vterm_screen_set_unrecognised_fallbacks(m_screen,&fallbacks,this);
    vterm_screen_enable_altscreen(m_screen,1);vterm_screen_enable_reflow(m_screen,true);
    vterm_screen_set_damage_merge(m_screen,VTERM_DAMAGE_SCREEN);
    vterm_output_set_callback(m_terminal,[](const char *data,size_t length,void *p){static_cast<TerminalModel*>(p)->sendOutput(QByteArray(data,qsizetype(length)));},this);
    setDarkTheme(false);vterm_screen_reset(m_screen,1);
}
TerminalModel::~TerminalModel(){vterm_free(m_terminal);}
void TerminalModel::feed(const QByteArray &bytes){vterm_input_write(m_terminal,bytes.constData(),size_t(bytes.size()));vterm_screen_flush_damage(m_screen);emit changed();}
bool TerminalModel::resize(QSize cells){if(cells.width()<2||cells.height()<1||cells.width()>500||cells.height()>300)return false;if(cells==m_size)return true;m_size=cells;vterm_set_size(m_terminal,cells.height(),cells.width());vterm_screen_flush_damage(m_screen);emit changed();return true;}
void TerminalModel::reset(){m_history.clear();++m_historyRevision;m_title.clear();m_osc.clear();vterm_screen_reset(m_screen,1);emit changed();}
void TerminalModel::sendOutput(const QByteArray &bytes){if(m_inputTransaction)m_pendingInput+=bytes;else emit outputReady(bytes);}
QPoint TerminalModel::cursor()const{VTermPos pos;vterm_state_get_cursorpos(m_state,&pos);return {pos.col,pos.row};}
TerminalModel::Cell TerminalModel::cell(int row,int column)const{
    Cell result;result.foreground=m_foreground;result.background=m_background;
    if(row<0||column<0||column>=m_size.width()||row>=totalLines())return result;
    VTermScreenCell raw{};
    if(row<historyLines()){
        const auto &line=m_history[size_t(row)];if(column>=line.size())return result;raw=line[column];
    }else if(!vterm_screen_get_cell(m_screen,{row-historyLines(),column},&raw))return result;
    if(raw.chars[0]==uint32_t(-1)){result.width=0;return result;}
    for(const auto c:raw.chars){if(!c)break;if(c<=0x10ffff&&!(c>=0xd800&&c<=0xdfff)){const char32_t cp=c;result.text+=QString::fromUcs4(&cp,1);}}
    result.width=qMax(1,int(raw.width));result.foreground=rgb(m_state,raw.fg);result.background=rgb(m_state,raw.bg);
    // Screen cells already include libvterm's global reverse-video flag.
    if(raw.attrs.reverse)std::swap(result.foreground,result.background);
    if(raw.attrs.conceal)result.foreground=result.background;
    result.bold=raw.attrs.bold;result.italic=raw.attrs.italic;result.underline=raw.attrs.underline;result.strike=raw.attrs.strike;return result;
}
QString TerminalModel::lineText(int row)const{QString result;for(int col=0;col<m_size.width();++col){const auto c=cell(row,col);if(c.width)result+=c.text.isEmpty()?QStringLiteral(" "):c.text;}while(result.endsWith(' '))result.chop(1);return result;}
QString TerminalModel::plainText(bool includeHistory)const{QStringList lines;for(int row=includeHistory?0:historyLines();row<totalLines();++row)lines<<lineText(row);while(!lines.isEmpty()&&lines.last().isEmpty())lines.removeLast();return lines.join('\n');}
int TerminalModel::property(VTermProp prop,VTermValue *value,void *context){auto *self=static_cast<TerminalModel*>(context);
    switch(prop){
    case VTERM_PROP_CURSORVISIBLE:self->m_cursorVisible=value->boolean;break;
    case VTERM_PROP_CURSORBLINK:self->m_cursorBlink=value->boolean;break;
    case VTERM_PROP_CURSORSHAPE:self->m_cursorShape=value->number;break;
    case VTERM_PROP_MOUSE:self->m_mouseMode=value->number;break;
    case VTERM_PROP_ALTSCREEN:self->m_alt=value->boolean;break;
    case VTERM_PROP_REVERSE:self->m_reverse=value->boolean;break;
    case VTERM_PROP_TITLE:{const auto fragment=value->string;if(fragment.initial)self->m_title.clear();if(self->m_title.size()+qsizetype(fragment.len)<=4096)self->m_title.append(fragment.str,fragment.len);if(fragment.final)emit self->titleChanged(QString::fromUtf8(self->m_title));break;}
    default:break;
    }return 1;
}
int TerminalModel::pushLine(int columns,const VTermScreenCell *cells,void *context){auto *self=static_cast<TerminalModel*>(context);self->m_history.emplace_back(cells,cells+columns);if(self->m_history.size()>HistoryLimit){self->m_history.pop_front();++self->m_historyRevision;}return 1;}
int TerminalModel::popLine(int columns,VTermScreenCell *cells,void *context){auto *self=static_cast<TerminalModel*>(context);if(self->m_history.empty())return 0;auto line=std::move(self->m_history.back());self->m_history.pop_back();std::memset(cells,0,size_t(columns)*sizeof(*cells));for(int i=0;i<columns;++i){if(i<line.size())cells[i]=line[i];else{cells[i].width=1;cells[i].fg=vtColor(self->m_foreground);cells[i].bg=vtColor(self->m_background);}}return 1;}
int TerminalModel::clearHistory(void *context){auto *self=static_cast<TerminalModel*>(context);self->m_history.clear();++self->m_historyRevision;return 1;}
int TerminalModel::osc(int command,VTermStringFragment fragment,void *context){auto *self=static_cast<TerminalModel*>(context);
    if(command!=7&&command!=11)return 0;
    if(fragment.initial)self->m_osc.clear();
    if(self->m_osc.size()+qsizetype(fragment.len)<=4096)self->m_osc.append(fragment.str,fragment.len);
    if(fragment.final){if(command==7){const QUrl url(QString::fromUtf8(self->m_osc));if(url.isLocalFile())emit self->workingDirectoryChanged(url.toLocalFile());}
        else if(self->m_osc=="?"){const auto c=self->m_background;const QByteArray response=QString("\x1b]11;rgb:%1/%2/%3\x07").arg(c.red()*257,4,16,QChar('0')).arg(c.green()*257,4,16,QChar('0')).arg(c.blue()*257,4,16,QChar('0')).toLatin1();emit self->outputReady(response);}}
    return 1;
}
void TerminalModel::key(int qtKey,Qt::KeyboardModifiers mods,const QString &text){VTermKey key=VTERM_KEY_NONE;
    // Windows layouts report AltGr as Ctrl+Alt. Preserve its printable text.
    if(mods.testFlag(Qt::GroupSwitchModifier)||(mods.testFlag(Qt::ControlModifier)&&mods.testFlag(Qt::AltModifier)&&!text.isEmpty()&&text.front().isPrint()))
        mods&=~(Qt::ControlModifier|Qt::AltModifier|Qt::GroupSwitchModifier);
    switch(qtKey){case Qt::Key_Return:case Qt::Key_Enter:key=VTERM_KEY_ENTER;break;case Qt::Key_Tab:case Qt::Key_Backtab:key=VTERM_KEY_TAB;break;case Qt::Key_Backspace:key=VTERM_KEY_BACKSPACE;break;case Qt::Key_Escape:key=VTERM_KEY_ESCAPE;break;case Qt::Key_Up:key=VTERM_KEY_UP;break;case Qt::Key_Down:key=VTERM_KEY_DOWN;break;case Qt::Key_Left:key=VTERM_KEY_LEFT;break;case Qt::Key_Right:key=VTERM_KEY_RIGHT;break;case Qt::Key_Insert:key=VTERM_KEY_INS;break;case Qt::Key_Delete:key=VTERM_KEY_DEL;break;case Qt::Key_Home:key=VTERM_KEY_HOME;break;case Qt::Key_End:key=VTERM_KEY_END;break;case Qt::Key_PageUp:key=VTERM_KEY_PAGEUP;break;case Qt::Key_PageDown:key=VTERM_KEY_PAGEDOWN;break;default:if(qtKey>=Qt::Key_F1&&qtKey<=Qt::Key_F35)key=VTermKey(VTERM_KEY_FUNCTION(qtKey-Qt::Key_F1+1));break;}
    if(qtKey==Qt::Key_Backtab)mods|=Qt::ShiftModifier;
    if(key!=VTERM_KEY_NONE){vterm_keyboard_key(m_terminal,key,modifiers(mods));return;}
    // Qt commonly supplies a control byte for Ctrl+C. Use the logical key so
    // libvterm performs the control mapping exactly once.
    if(mods.testFlag(Qt::ControlModifier)&&qtKey>=Qt::Key_A&&qtKey<=Qt::Key_Z){vterm_keyboard_unichar(m_terminal,uint32_t('a'+qtKey-Qt::Key_A),modifiers(mods));return;}
    for(const char32_t cp:text.toUcs4())vterm_keyboard_unichar(m_terminal,cp,modifiers(mods));
}
void TerminalModel::paste(const QString &text){
    m_inputTransaction=true;m_pendingInput.clear();
    vterm_keyboard_start_paste(m_terminal);
    QString normalized;normalized.reserve(text.size());
    // Clipboard text must not break out of bracketed paste with ESC[201~.
    for(const auto c:text)if(c.unicode()>=0x20||c=='\t'||c=='\r'||c=='\n')normalized+=c;
    normalized.replace("\r\n","\n");normalized.replace('\r','\n');normalized.replace('\n','\r');
    sendOutput(normalized.toUtf8());vterm_keyboard_end_paste(m_terminal);
    m_inputTransaction=false;const auto bytes=std::move(m_pendingInput);emit outputReady(bytes);
}
void TerminalModel::focus(bool focused){if(focused)vterm_state_focus_in(m_state);else vterm_state_focus_out(m_state);}
void TerminalModel::mouseMove(int row,int column,Qt::KeyboardModifiers mods){vterm_mouse_move(m_terminal,row,column,modifiers(mods));}
void TerminalModel::mouseButton(int button,bool pressed,Qt::KeyboardModifiers mods){vterm_mouse_button(m_terminal,button,pressed,modifiers(mods));}
void TerminalModel::setDarkTheme(bool dark){
    m_background=QColor(dark?"#1e1e1d":"#fdfcfc");m_foreground=QColor(dark?"#e0e0e0":"#1a1a1a");
    const auto fg=vtColor(m_foreground),bg=vtColor(m_background);vterm_screen_set_default_colors(m_screen,&fg,&bg);
    const double hues[]={85,25,152,65,220,300,175,85};
    const double chromaLight[]={.002,.14,.16,.17,.16,.18,.15,.002},chromaDark[]={.002,.16,.14,.15,.14,.14,.10,.002};
    const double brightLight[]={.002,.2,.2,.2,.2,.2,.17,.002},brightDark[]={.002,.18,.16,.17,.16,.16,.12,.002};
    for(int index=0;index<16;++index){const int i=index%8;const bool bright=index>=8;const double l=i==0?(bright?(dark?.45:.65):(dark?.25:.92)):i==7?(bright?(dark?.92:.2):(dark?.75:.35)):(bright?(dark?.7:.5):(dark?.55:.45));const auto color=vtColor(oklch(l,bright?(dark?brightDark[i]:brightLight[i]):(dark?chromaDark[i]:chromaLight[i]),hues[i]));vterm_state_set_palette_color(m_state,index,&color);}
    emit changed();
}
}
