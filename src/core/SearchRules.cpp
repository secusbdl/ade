#include "SearchRules.h"
#include <optional>
#include <utility>
#include <vector>

namespace ade::search {
namespace {
constexpr qsizetype MaxGlobLength=4096;
constexpr std::size_t MaxAlternateDepth=32;
struct Token {
    enum Kind { Literal,Any,ZeroOrMore,RecursivePrefix,RecursiveSuffix,RecursiveZeroOrMore,Class,Alternates } kind=Literal;
    char32_t ch=0;
    bool negated=false;
    std::vector<std::pair<char32_t,char32_t>> ranges;
    std::vector<std::vector<Token>> alternates;
};
using Tokens=std::vector<Token>;
Token token(Token::Kind kind,char32_t ch=0){Token result;result.kind=kind;result.ch=ch;return result;}

// Port of globset's parser with literal_separator and backslash_escape on,
// the options the ignore crate uses for ignore lines and override globs.
// Character classes may match '/', as rg does.
class Parser {
public:
    Parser(const QString& glob,bool allowUnclosedClass):m_allowUnclosedClass(allowUnclosedClass){
        for(const auto c:glob.toUcs4())m_chars.push_back(char32_t(c));
        m_groups.push_back({Tokens{}});
    }
    bool parse(Tokens* result,QString* error){
        m_error=error;
        while(const auto c=bump()){
            bool ok=true;
            switch(*c){
            case U'?':push(token(Token::Any));break;
            case U'*':ok=parseStar();break;
            case U'[':
                if(m_foundUnclosedClass)push(token(Token::Literal,U'['));
                else ok=parseClass();
                break;
            case U'{':
                if(m_groups.size()>MaxAlternateDepth)ok=fail(QStringLiteral("alternate groups are nested too deeply"));
                else m_groups.push_back({Tokens{}});
                break;
            case U'}':ok=popAlternate();break;
            case U',':
                if(m_groups.size()>1)m_groups.back().push_back({});
                else push(token(Token::Literal,U','));
                break;
            case U'\\':{
                const auto next=bump();
                if(!next)ok=fail(QStringLiteral("dangling '\\'"));
                else push(token(Token::Literal,*next));
                break;
            }
            default:push(token(Token::Literal,*c));
            }
            if(!ok)return false;
        }
        if(m_groups.size()>1)return fail(QStringLiteral("unclosed alternate group; missing '}'"));
        *result=std::move(m_groups.front().front());
        return true;
    }
private:
    std::optional<char32_t> bump(){
        m_prev=m_cur;
        if(m_position>=m_chars.size())m_cur.reset();
        else m_cur=m_chars[m_position++];
        return m_cur;
    }
    std::optional<char32_t> peek() const{
        if(m_position>=m_chars.size())return std::nullopt;
        return m_chars[m_position];
    }
    Tokens& branch(){return m_groups.back().back();}
    void push(Token value){branch().push_back(std::move(value));}
    bool fail(const QString& reason){if(m_error)*m_error=reason;return false;}
    bool popAlternate(){
        if(m_groups.size()<=1)return fail(QStringLiteral("unopened alternate group; missing '{'"));
        Token group=token(Token::Alternates);
        group.alternates=std::move(m_groups.back());
        m_groups.pop_back();
        push(std::move(group));
        return true;
    }
    bool parseStar(){
        const auto prev=m_prev;
        if(peek()!=U'*'){push(token(Token::ZeroOrMore));return true;}
        bump();
        if(branch().empty()){
            const auto next=peek();
            if(next&&*next!=U'/'){
                push(token(Token::ZeroOrMore));
                push(token(Token::ZeroOrMore));
            }else{
                push(token(Token::RecursivePrefix));
                bump();
            }
            return true;
        }
        if(prev!=U'/'){
            if(m_groups.size()<=1||(prev!=U','&&prev!=U'{')){
                push(token(Token::ZeroOrMore));
                push(token(Token::ZeroOrMore));
                return true;
            }
        }
        bool suffix=false;
        const auto next=peek();
        if(!next){
            bump();
            suffix=true;
        }else if((*next==U','||*next==U'}')&&m_groups.size()>1){
            suffix=true;
        }else if(*next==U'/'){
            bump();
        }else{
            push(token(Token::ZeroOrMore));
            push(token(Token::ZeroOrMore));
            return true;
        }
        const auto previous=branch().back().kind;
        branch().pop_back();
        if(previous==Token::RecursivePrefix||previous==Token::RecursiveSuffix)push(token(previous));
        else push(token(suffix?Token::RecursiveSuffix:Token::RecursiveZeroOrMore));
        return true;
    }
    bool extendRange(std::pair<char32_t,char32_t>* range,char32_t end){
        range->second=end;
        if(range->second<range->first)return fail(QStringLiteral("invalid character range"));
        return true;
    }
    bool parseClass(){
        const auto savedPosition=m_position;
        const auto savedPrev=m_prev,savedCur=m_cur;
        Token result=token(Token::Class);
        if(const auto next=peek();next&&(*next==U'!'||*next==U'^')){
            bump();
            result.negated=true;
        }
        bool first=true,inRange=false;
        while(true){
            const auto c=bump();
            if(!c){
                if(m_allowUnclosedClass){
                    m_position=savedPosition;
                    m_prev=savedPrev;
                    m_cur=savedCur;
                    m_foundUnclosedClass=true;
                    push(token(Token::Literal,U'['));
                    return true;
                }
                return fail(QStringLiteral("unclosed character class; missing ']'"));
            }
            if(*c==U']'){
                if(!first)break;
                result.ranges.push_back({U']',U']'});
            }else if(*c==U'-'){
                if(first){
                    result.ranges.push_back({U'-',U'-'});
                }else if(inRange){
                    if(!extendRange(&result.ranges.back(),U'-'))return false;
                    inRange=false;
                }else{
                    inRange=true;
                }
            }else{
                if(inRange){
                    if(!extendRange(&result.ranges.back(),*c))return false;
                }else{
                    result.ranges.push_back({*c,*c});
                }
                inRange=false;
            }
            first=false;
        }
        if(inRange)result.ranges.push_back({U'-',U'-'});
        push(std::move(result));
        return true;
    }
    std::vector<char32_t> m_chars;
    std::size_t m_position=0;
    std::optional<char32_t> m_prev,m_cur;
    std::vector<std::vector<Tokens>> m_groups;
    bool m_allowUnclosedClass=false,m_foundUnclosedClass=false;
    QString* m_error=nullptr;
};

QString literal(char32_t c){return QRegularExpression::escape(QString::fromUcs4(&c,1));}
QString classMember(char32_t c){return QStringLiteral("\\x{%1}").arg(uint(c),0,16);}
void appendRegex(const Tokens& tokens,QString* regex){
    for(const auto& item:tokens){
        switch(item.kind){
        case Token::Literal:*regex+=literal(item.ch);break;
        case Token::Any:*regex+=QStringLiteral("[^/]");break;
        case Token::ZeroOrMore:*regex+=QStringLiteral("[^/]*");break;
        case Token::RecursivePrefix:*regex+=QStringLiteral("(?:/?|.*/)");break;
        case Token::RecursiveSuffix:*regex+=QStringLiteral("/.*");break;
        case Token::RecursiveZeroOrMore:*regex+=QStringLiteral("(?:/|/.*/)");break;
        case Token::Class:
            *regex+=item.negated?QStringLiteral("[^"):QStringLiteral("[");
            for(const auto& range:item.ranges){
                *regex+=classMember(range.first);
                if(range.second!=range.first)*regex+=QStringLiteral("-")+classMember(range.second);
            }
            *regex+=QStringLiteral("]");
            break;
        case Token::Alternates:{
            // globset drops empty alternatives: "x{a,}" never matches "x".
            QStringList parts;
            for(const auto& alternative:item.alternates){
                QString part;
                appendRegex(alternative,&part);
                if(!part.isEmpty())parts.append(part);
            }
            if(!parts.isEmpty())*regex+=QStringLiteral("(?:")+parts.join(QStringLiteral("|"))+QStringLiteral(")");
            break;
        }
        }
    }
}
bool compileGlob(const QString& glob,bool allowUnclosedClass,QRegularExpression* result,QString* error){
    Tokens tokens;
    if(!Parser(glob,allowUnclosedClass).parse(&tokens,error))return false;
    QString pattern=QStringLiteral("^");
    if(tokens.size()==1&&tokens.front().kind==Token::RecursivePrefix)pattern+=QStringLiteral(".*");
    else appendRegex(tokens,&pattern);
    pattern+=QStringLiteral("\\z");
    QRegularExpression regex(pattern);
    if(!regex.isValid()){*error=regex.errorString();return false;}
    *result=std::move(regex);
    return true;
}
}

// Port of the ignore crate's GitignoreBuilder::add_line. Override globs use
// the same parser in rg, so "#x" is a comment there as well.
bool compileRule(const QString& text,Syntax syntax,int line,Rule* rule,QString* error){
    error->clear();
    *rule={};
    rule->line=line;
    if(text.size()>MaxGlobLength){*error=QStringLiteral("pattern exceeds 4096 characters");return false;}
    QString glob=text;
    if(glob.startsWith(u'#'))return false;
    if(!glob.endsWith(QStringLiteral("\\ ")))while(!glob.isEmpty()&&glob.back().isSpace())glob.chop(1);
    if(glob.isEmpty())return false;
    bool absolute=false;
    if(glob.startsWith(QStringLiteral("\\!"))||glob.startsWith(QStringLiteral("\\#"))){
        glob.remove(0,1);
        absolute=glob.startsWith(u'/');
    }else{
        if(glob.startsWith(u'!')){rule->negated=true;glob.remove(0,1);}
        if(glob.startsWith(u'/')){glob.remove(0,1);absolute=true;}
    }
    if(glob.endsWith(u'/')){
        rule->directoryOnly=true;
        glob.chop(1);
        if(glob.endsWith(u'\\'))glob.chop(1);
    }
    // A glob without any slash matches a name at any depth.
    if(!absolute&&!glob.contains(u'/')&&glob!=QStringLiteral("**"))glob.prepend(QStringLiteral("**/"));
    // "dir/**" matches everything inside dir, never dir itself.
    const bool recursiveSuffix=glob.endsWith(QStringLiteral("/**"));
    const QString prefix=recursiveSuffix?glob.chopped(3):QString();
    if(recursiveSuffix)glob+=QStringLiteral("/*");
    if(!compileGlob(glob,syntax==Syntax::IgnoreFile,&rule->regex,error))return false;
    if(syntax==Syntax::Override&&recursiveSuffix&&!rule->directoryOnly){
        QString ignored;
        rule->prunes=compileGlob(prefix,false,&rule->descendants,&ignored);
    }
    return true;
}

Match RuleFile::match(const QString& mountPath,bool isDirectory) const{
    QStringView relative(mountPath);
    if(!base.isEmpty()){
        if(mountPath.size()<=base.size()+1||!mountPath.startsWith(base)||mountPath.at(base.size())!=u'/')return Match::None;
        relative=relative.mid(base.size()+1);
    }
    for(auto it=rules.crbegin();it!=rules.crend();++it){
        if(it->directoryOnly&&!isDirectory)continue;
        if(it->regex.matchView(relative).hasMatch())return it->negated?Match::Whitelist:Match::Ignore;
    }
    return Match::None;
}

QString ignoreFileName(IgnoreKind kind){
    switch(kind){
    case IgnoreKind::RgIgnore:return QStringLiteral(".rgignore");
    case IgnoreKind::Ignore:return QStringLiteral(".ignore");
    case IgnoreKind::GitIgnore:break;
    }
    return QStringLiteral(".gitignore");
}

Match IgnoreStack::match(const QString& mountPath,const QString& name,bool isDirectory) const{
    for(int kind=0;kind<IgnoreKindCount;++kind){
        for(auto it=m_levels.crbegin();it!=m_levels.crend();++it){
            if(it->present[kind]){
                if(const auto result=it->files[kind].match(mountPath,isDirectory);result!=Match::None)return result;
            }
            if(kind==int(IgnoreKind::GitIgnore)&&it->gitBoundary)break;
        }
    }
    if(isDirectory&&name==QStringLiteral("node_modules"))return Match::Ignore;
    return Match::None;
}

bool OverrideSet::add(const QString& glob,QString* error){
    Rule rule;
    if(!compileRule(glob,Syntax::Override,0,&rule,error))return error->isEmpty();
    if(!rule.negated)++whitelists;
    rules.append(std::move(rule));
    return true;
}

Match OverrideSet::match(const QString& searchPath,bool isDirectory) const{
    for(auto it=rules.crbegin();it!=rules.crend();++it){
        if(it->directoryOnly&&!isDirectory)continue;
        if(it->regex.match(searchPath).hasMatch())return it->negated?Match::Ignore:Match::Whitelist;
    }
    // With any include glob, files that match none of them are dropped.
    return whitelists>0&&!isDirectory?Match::Ignore:Match::None;
}

bool OverrideSet::prunes(const QString& searchDirectory) const{
    // Exact shortcut for "!dir/**": every descendant is excluded unless a
    // later include glob could still match it.
    for(auto it=rules.crbegin();it!=rules.crend();++it){
        if(!it->negated)return false;
        if(it->prunes&&it->descendants.match(searchDirectory).hasMatch())return true;
    }
    return false;
}

bool admit(const Policy& policy,const OverrideSet& overrides,const IgnoreStack& ignores,
           const QString& name,const QString& searchPath,const QString& mountPath,
           bool isDirectory,bool isReparsePoint,bool isHiddenAttribute){
    if(isReparsePoint||name==QStringLiteral(".git"))return false;
    if(isDirectory&&overrides.prunes(searchPath))return false;
    const auto selected=overrides.match(searchPath,isDirectory);
    if(selected==Match::Ignore)return false;
    if(selected==Match::Whitelist)return true;
    bool whitelisted=false;
    if(policy.respectIgnores){
        const auto ignored=ignores.match(mountPath,name,isDirectory);
        if(ignored==Match::Ignore)return false;
        whitelisted=ignored==Match::Whitelist;
    }
    if(!whitelisted&&policy.skipHidden&&(name.startsWith(u'.')||isHiddenAttribute))return false;
    return true;
}
}
