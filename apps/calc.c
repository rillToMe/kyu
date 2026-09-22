// ============================================================
// calc.c — Kalkulator (Phase 10): operasi +,-,×,÷,%,+/- (libui).
// Display Label + grid tombol (HBox per baris). State machine
// diambil utuh dari versi libgui sebelumnya.
//
// Build: calc.o + userlib.o + libgui.o + libui.o + png.o
// ============================================================
#include "userlib.h"
#include "libui.h"

#define BTN_W   68
#define BTN_H   48
#define BTN_PAD 6

// --- State kalkulator ---
static double accumulator = 0;
static double input_val   = 0;
static int    op          = 0;   // 0=none 1=+ 2=- 3=* 4=/
static int    has_input   = 0;
static int    decimal     = 0;
static int    dec_place   = 1;
static int    just_result = 0;

static ui_widget_t* g_disp;
static ui_widget_t* g_status;

static const char* btn_labels[5][4] = {
    { "C",  "+/-", "%",  "/" },
    { "7",  "8",   "9",  "*" },
    { "4",  "5",   "6",  "-" },
    { "1",  "2",   "3",  "+" },
    { "0",  ".",   "CE", "=" },
};

static void double_to_str(double v, char* buf, int maxlen) {
    int i = 0;
    if (maxlen < 2) { buf[0]='\0'; return; }
    if (v < 0) { buf[i++]='-'; v=-v; }
    if (v > 99999999.0) { buf[0]='E'; buf[1]='R'; buf[2]='R'; buf[3]='\0'; return; }
    int int_part = (int)v;
    double frac = v - (double)int_part;
    char tmp[16]; int ti=0;
    if (int_part == 0) tmp[ti++]='0';
    else { int n=int_part; while(n>0&&ti<15){tmp[ti++]='0'+(n%10);n/=10;} }
    for (int a=0,b=ti-1;a<b;a++,b--){char t=tmp[a];tmp[a]=tmp[b];tmp[b]=t;}
    for (int k=0;k<ti&&i<maxlen-1;k++) buf[i++]=tmp[k];
    if (frac > 0.0001) {
        if (i<maxlen-1) buf[i++]='.';
        int places=4;
        while (places-->0&&i<maxlen-1){frac*=10.0;int d=(int)frac;buf[i++]='0'+d;frac-=d;}
        while (i>1&&buf[i-1]=='0') i--;
        if (i>1&&buf[i-1]=='.') i--;
    }
    buf[i]='\0';
}

// --- Proses tombol ---
static void press(int col, int row) {
    const char* lbl = btn_labels[row][col];
    if (lbl[0]>='0'&&lbl[0]<='9'&&lbl[1]=='\0') {
        int d=lbl[0]-'0';
        if (just_result){input_val=0;accumulator=0;op=0;just_result=0;decimal=0;dec_place=1;}
        if (!has_input){input_val=0;decimal=0;dec_place=1;has_input=1;}
        if (!decimal) input_val=input_val*10.0+d;
        else { dec_place*=10; input_val+=((double)d/(double)dec_place); }
        return;
    }
    if (lbl[0]=='.'&&lbl[1]=='\0'){if(!has_input)has_input=1;if(!decimal){decimal=1;dec_place=1;}return;}
    if (lbl[0]=='C'&&lbl[1]=='E'){input_val=0;decimal=0;dec_place=1;has_input=0;return;}
    if (lbl[0]=='C'&&lbl[1]=='\0'){input_val=0;accumulator=0;op=0;decimal=0;dec_place=1;has_input=0;just_result=0;return;}
    if (lbl[0]=='+'&&lbl[1]=='/'){input_val=-input_val;return;}
    if (lbl[0]=='%'){input_val=(op!=0)?accumulator*input_val/100.0:input_val/100.0;has_input=1;return;}
    if (lbl[0]=='=') {
        if (op==0){just_result=1;return;}
        double r=0;
        switch(op){case 1:r=accumulator+input_val;break;case 2:r=accumulator-input_val;break;
                   case 3:r=accumulator*input_val;break;case 4:r=(input_val==0)?0:accumulator/input_val;break;}
        accumulator=r;input_val=r;op=0;has_input=0;decimal=0;dec_place=1;just_result=1;return;
    }
    int new_op=0;
    if (lbl[0]=='+'&&lbl[1]=='\0') new_op=1;
    else if (lbl[0]=='-') new_op=2;
    else if (lbl[0]=='*') new_op=3;
    else if (lbl[0]=='/') new_op=4;
    if (new_op) {
        if (op&&has_input){
            double r=0;
            switch(op){case 1:r=accumulator+input_val;break;case 2:r=accumulator-input_val;break;
                       case 3:r=accumulator*input_val;break;case 4:r=(input_val==0)?0:accumulator/input_val;break;}
            accumulator=r;input_val=r;
        } else accumulator=input_val;
        op=new_op;has_input=0;decimal=0;dec_place=1;just_result=0;
    }
}

static void refresh_disp(void) {
    char disp[24]; double_to_str(input_val, disp, 24);
    ui_label_set_text(g_disp, disp);

    char st[32]; int k = 0;
    const char* op_str = "";
    if (op==1) op_str="[+]"; else if(op==2) op_str="[-]";
    else if(op==3) op_str="[*]"; else if(op==4) op_str="[/]";
    if (op!=0 && !just_result) {
        char acc[24]; double_to_str(accumulator, acc, 24);
        for (int i = 0; acc[i] && k < 14; i++) st[k++] = acc[i];
        for (int i = 0; op_str[i] && k < 20; i++) st[k++] = op_str[i];
    }
    st[k] = '\0';
    ui_label_set_text(g_status, st);
}

static void on_btn(void* userdata) {
    int key = (int)(uintptr_t)userdata;
    press(key % 4, key / 4);
    refresh_disp();
}

void main(void) {
    ui_window_t* win = ui_window_create(320, 400);
    if (!win) { sys_exit(); }
    ui_window_set_title(win, "Kalkulator");

    ui_widget_t* box = ui_vbox_create(win, 6);
    g_status = ui_label_create(win, "");
    ui_layout_add(box, g_status);
    g_disp = ui_label_create(win, "0");
    ui_widget_set_size(g_disp, 300, 32);
    ui_layout_add(box, g_disp);

    for (int r = 0; r < 5; r++) {
        ui_widget_t* row = ui_hbox_create(win, BTN_PAD);
        for (int c = 0; c < 4; c++) {
            ui_widget_t* b = ui_button_create(win, btn_labels[r][c]);
            ui_widget_set_size(b, BTN_W, BTN_H);
            ui_button_set_click(b, on_btn, (void*)(uintptr_t)(r * 4 + c));
            ui_layout_add(row, b);
        }
        ui_layout_add(box, row);
    }

    ui_window_add(win, box);
    refresh_disp();
    ui_window_run(win);   // blocking; keluar via X / ESC
    ui_window_destroy(win);
    sys_exit();
}
