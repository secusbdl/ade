#include "Icons.h"
#include <QPainter>
#include <QPainterPath>
namespace ade {
QIcon icon(const QString& name, const QColor& color) {
    QPixmap pixmap(48,48); pixmap.fill(Qt::transparent);
    QPainter p(&pixmap); p.setRenderHint(QPainter::Antialiasing); p.scale(2,2);
    p.setPen(QPen(color,1.6,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
    auto line=[&](int x,int y,int a,int b){p.drawLine(x,y,a,b);};
    if(name=="plus" || name=="new") { line(12,5,12,19);line(5,12,19,12); }
    else if(name=="search") { p.drawEllipse(QRectF(4,4,11,11));line(14,14,20,20); }
    else if(name=="back") { line(17,12,5,12);line(5,12,10,7);line(5,12,10,17); }
    else if(name=="forward") { line(7,12,19,12);line(19,12,14,7);line(19,12,14,17); }
    else if(name=="reload") { p.drawArc(QRectF(5,5,14,14),40*16,285*16);line(19,4,19,10);line(19,10,13,10); }
    else if(name=="send") {line(12,19,12,5);line(6,11,12,5);line(12,5,18,11);}
    else if(name=="download") {line(12,3,12,15);line(7,10,12,15);line(12,15,17,10);line(4,16,4,21);line(4,21,20,21);line(20,21,20,16);}
    else if(name=="stop") {p.setBrush(color);p.drawRoundedRect(QRectF(7,7,10,10),1,1);}
    else if(name=="close") {line(7,7,17,17);line(17,7,7,17);}
    else if(name=="select") {QPainterPath a;a.moveTo(5,3);a.lineTo(5,20);a.lineTo(10,15);a.lineTo(15,21);a.lineTo(18,19);a.lineTo(13,13);a.lineTo(20,11);a.closeSubpath();p.drawPath(a);}
    else if(name=="camera") {p.drawRoundedRect(QRectF(3,6,18,14),2,2);p.drawEllipse(QRectF(8,9,8,8));line(8,6,10,3);line(10,3,16,3);line(16,3,18,6);}
    else if(name=="device") {p.drawRoundedRect(QRectF(6,2,12,20),2,2);line(10,18,14,18);}
    else if(name=="code") {line(8,6,2,12);line(2,12,8,18);line(16,6,22,12);line(22,12,16,18);line(14,4,10,20);}
    else if(name=="folder") {QPainterPath a;a.moveTo(3,7);a.lineTo(9,7);a.lineTo(11,9);a.lineTo(21,9);a.lineTo(21,19);a.lineTo(3,19);a.closeSubpath();p.drawPath(a);}
    else if(name=="terminal") {line(5,7,10,12);line(10,12,5,17);line(12,18,19,18);}
    else if(name=="sidebar") {p.drawRoundedRect(QRectF(3,5,18,14),2,2);line(9,5,9,19);line(15,9,12,12);line(12,12,15,15);}
    else if(name=="globe") {p.drawEllipse(QRectF(4,4,16,16));p.drawEllipse(QRectF(8,4,8,16));line(4,12,20,12);}
    else if(name=="settings") {p.drawEllipse(QRectF(8,8,8,8));for(int i=0;i<8;++i){p.save();p.translate(12,12);p.rotate(i*45);p.drawLine(0,8,0,10);p.restore();}}
    else if(name=="attach") {QPainterPath a;a.moveTo(8,14);a.lineTo(14,8);a.cubicTo(18,4,22,8,18,12);a.lineTo(10,20);a.cubicTo(5,25,-1,18,4,13);a.lineTo(13,4);p.translate(1,-1);p.scale(.9,.9);p.drawPath(a);}
    else if(name=="save") {p.drawRect(QRectF(4,4,16,16));p.drawRect(QRectF(8,4,8,6));p.drawRect(QRectF(8,14,8,6));}
    else if(name=="document") {p.drawRoundedRect(QRectF(5,3,14,18),1,1);line(9,9,15,9);line(9,13,15,13);line(9,17,13,17);}
    else if(name=="branch") {p.drawEllipse(QRectF(3,3,4,4));p.drawEllipse(QRectF(3,17,4,4));p.drawEllipse(QRectF(17,3,4,4));line(5,7,5,17);QPainterPath a;a.moveTo(19,7);a.cubicTo(19,14,5,10,5,17);p.drawPath(a);}
    else if(name=="archive") {p.drawRect(QRectF(3,4,18,4));p.drawRect(QRectF(5,8,14,12));line(10,12,14,12);}
    else if(name=="trash") {line(4,6,20,6);line(9,3,15,3);p.drawRoundedRect(QRectF(6,6,12,15),1,1);line(10,10,10,17);line(14,10,14,17);}
    else if(name=="clock") {p.drawEllipse(QRectF(3,3,18,18));line(12,7,12,12);line(12,12,16,14);}
    else if(name=="user") {p.drawEllipse(QRectF(8,3,8,8));p.drawArc(QRectF(5,14,14,14),0,180*16);}
    else if(name=="info") {p.drawEllipse(QRectF(3,3,18,18));p.drawPoint(12,7);line(12,11,12,17);}
    else if(name=="palette") {p.drawEllipse(QRectF(3,3,18,18));p.setBrush(color);for(const auto& pt:{QPointF(8,7),QPointF(15,7),QPointF(18,12),QPointF(7,13)})p.drawEllipse(pt,1,1);p.setBrush(Qt::NoBrush);p.drawEllipse(QRectF(10,15,4,4));}
    else if(name=="brain") {QPainterPath a;a.moveTo(11,5);a.cubicTo(7,1,4,5,5,8);a.cubicTo(1,10,3,15,5,15);a.cubicTo(3,20,8,23,11,19);a.closeSubpath();p.drawPath(a);p.save();p.translate(24,0);p.scale(-1,1);p.drawPath(a);p.restore();line(7,9,10,11);line(7,17,11,15);line(17,9,14,11);line(17,17,13,15);}
    else if(name=="plugin") {QPainterPath a;a.moveTo(4,9);a.lineTo(9,9);a.cubicTo(6,1,18,1,15,9);a.lineTo(20,9);a.lineTo(20,14);a.cubicTo(12,11,12,23,20,20);a.lineTo(20,21);a.lineTo(4,21);a.lineTo(4,16);a.cubicTo(12,19,12,7,4,10);a.closeSubpath();p.drawPath(a);}
    else {p.drawEllipse(QRectF(5,5,14,14));p.setBrush(color);p.drawEllipse(QRectF(9,9,6,6));}
    return QIcon(pixmap);
}
}
