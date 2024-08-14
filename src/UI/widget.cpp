#include "widget.hpp"

Widget::Widget(){

}

Widget::Widget(const char* name_) : 
    name(name_)
{
    
}

void Widget::Render(){
    Draw();
}

void Widget::Draw(){
}

Widget::~Widget(){

}