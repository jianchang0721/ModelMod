package com.moss.modelmod;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.widget.LinearLayout;
import android.widget.TextView;

public class MainActivity extends Activity {

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);

        LinearLayout ll = new LinearLayout(this);
        ll.setOrientation(LinearLayout.VERTICAL);
        ll.setBackgroundColor(0xFF0E0E16);
        ll.setPadding(60, 120, 60, 60);

        TextView title = new TextView(this);
        title.setText("ModelMod");
        title.setTextColor(0xFFFFD54F);
        title.setTextSize(28);
        title.setGravity(Gravity.START);
        ll.addView(title);

        TextView body = new TextView(this);
        body.setText("\n状态检查\n"
                + "──────────────\n"
                + "• LSPosed 里启用本模块\n"
                + "• 作用域勾选「航海王热血航线」\n"
                + "• 强制停止游戏后重开\n"
                + "\n使用方法\n"
                + "──────────────\n"
                + "进入游戏约 6 秒后，屏幕左上角\n"
                + "出现 ModelMod 面板。\n"
                + "拖动标题移动位置，\n"
                + "勾选开关即时生效。\n"
                + "\n部位说明\n"
                + "──────────────\n"
                + "身体  角色主体网格\n"
                + "脸    面部网格\n"
                + "头发  发型网格\n"
                + "武器  手持武器\n"
                + "描边  _PX 轮廓层\n"
                + "NPC   非玩家角色\n");
        body.setTextColor(Color.WHITE);
        body.setTextSize(15);
        ll.addView(body);

        setContentView(ll);
    }
}
