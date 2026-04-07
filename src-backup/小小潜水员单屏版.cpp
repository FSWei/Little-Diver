#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <U8g2_for_Adafruit_GFX.h>

// =====================
// 硬件配置
// =====================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

#define SDA_PIN 1
#define SCL_PIN 2
#define AO_PIN 10
#define DO_PIN 9

// =====================
// 传感器校准（根据实际传感器调整）
// =====================
#define SENSOR_DRY 2800 // 干燥 ADC 值 → 海面（深度 0）
#define SENSOR_WET 1200 // 湿润 ADC 值 → 海底（深度 100）

// =====================
// 世界坐标系说明
//   worldY：垂直深度，0=海面，WORLD_MAX_DEPTH=海底
//           允许负值（潜水员浮出水面时 worldY<0）
//   worldX：水平距离，随时间持续增加（潜水员向右游）
// =====================
#define WORLD_MAX_DEPTH 200

// =====================
// 屏幕布局
// =====================
#define GAME_H 55 // 游戏区高度（底部留 9px 给 HUD）
// 海面(worldY=0)对应屏幕 Y = SURFACE_SCREEN_Y
// 这样在最浅时，海面在屏幕中间，角色可以浮出水面看到天空
#define SURFACE_SCREEN_Y 24

// =====================
// 摄像机边界（角色到达此距离才滚动摄像头）
// =====================
#define CAM_MARGIN_TOP 12
#define CAM_MARGIN_BOTTOM 14
#define CAM_MARGIN_LEFT 24
#define CAM_MARGIN_RIGHT 52

// =====================
// 游戏对象数量
// =====================
#define MAX_JELLYFISH 5
#define MAX_TREASURES 6
#define BUBBLE_COUNT 10
#define SEAWEED_COUNT 8

#define CHAOS_DURATION 80

// 潜水员水平游速（世界像素×8/帧）
#define DIVER_SPEED_X8 10 // ~1.25 像素/帧

// 帧率目标
#define TARGET_FPS_MS 28 // ~35 FPS

// =====================
// 全局对象
// =====================
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;

// =====================
// 数据结构
// =====================
struct WorldObj
{
    int wx, wy;
    int vx, vy;
    bool active;
    int tier;
};

struct Bubble
{
    int wx, wy;
    int riseSpd;  // 上升速度
    int driftSpd; // 向左漂移速度（视差）
};

struct Seaweed
{
    int wx, wy;
    int h;
    int phase;
};

// =====================
// 游戏状态
// =====================
enum GameState
{
    STATE_TITLE,
    STATE_PLAY,
    STATE_GAMEOVER
};

GameState gState = STATE_TITLE;
int gScore = 0;
int gHighScore = 0;
int gLives = 3;
int gChaos = 0;
unsigned long gFrameCount = 0;
unsigned long gLastMs = 0;

// 潜水员世界坐标（×8 定点数，支持亚像素平滑）
int gDiverWX8 = 0;
int gDiverWY8 = 0;

// 目标深度（世界像素）
int gTargetWY = 0;

// 摄像机左上角世界坐标
int gCamWX = 0;
int gCamWY = 0;

// 海草生成基准 X
int gSeaweedSpawnX = 0;

WorldObj gJellyfish[MAX_JELLYFISH];
WorldObj gTreasures[MAX_TREASURES];
Bubble gBubbles[BUBBLE_COUNT];
Seaweed gSeaweed[SEAWEED_COUNT];

// =====================
// 工具函数
// =====================
int randRange(int lo, int hi)
{
    if (hi <= lo)
        return lo;
    return lo + (int)(esp_random() % (unsigned int)(hi - lo + 1));
}

// 快速传感器读取（2次均值，减少延迟）
int readMoisture()
{
    return (analogRead(AO_PIN) + analogRead(AO_PIN)) >> 1;
}

int moistureToDepthPct(int raw)
{
    return constrain(map(raw, SENSOR_DRY, SENSOR_WET, 0, 100), 0, 100);
}

// 世界坐标 → 屏幕坐标
inline int w2sx(int wx) { return wx - gCamWX; }
inline int w2sy(int wy) { return wy - gCamWY; }

// =====================
// 初始化
// =====================
void initJellyfish()
{
    for (int i = 0; i < MAX_JELLYFISH; i++)
    {
        gJellyfish[i].active = true;
        gJellyfish[i].wx = randRange(80, 400);
        gJellyfish[i].wy = randRange(10, WORLD_MAX_DEPTH - 10);
        gJellyfish[i].vx = randRange(-6, 6);
        gJellyfish[i].vy = randRange(-4, 4);
    }
}

void initTreasures()
{
    static const int tScores[MAX_TREASURES] = {10, 20, 30, 50, 80, 150};
    for (int i = 0; i < MAX_TREASURES; i++)
    {
        int yLo = i * (WORLD_MAX_DEPTH / MAX_TREASURES);
        int yHi = yLo + WORLD_MAX_DEPTH / MAX_TREASURES - 5;
        gTreasures[i].active = true;
        gTreasures[i].wx = randRange(80 + i * 55, 130 + i * 55);
        gTreasures[i].wy = randRange(yLo + 3, yHi);
        gTreasures[i].tier = i;
    }
}

void initBubbles()
{
    for (int i = 0; i < BUBBLE_COUNT; i++)
    {
        gBubbles[i].wx = randRange(-50, 200);
        gBubbles[i].wy = randRange(2, WORLD_MAX_DEPTH - 2);
        gBubbles[i].riseSpd = randRange(1, 3);
        gBubbles[i].driftSpd = randRange(1, 3);
    }
}

void initSeaweed()
{
    for (int i = 0; i < SEAWEED_COUNT; i++)
    {
        gSeaweed[i].wx = randRange(-10, 320);
        gSeaweed[i].wy = WORLD_MAX_DEPTH - randRange(0, 5);
        gSeaweed[i].h = randRange(7, 18);
        gSeaweed[i].phase = randRange(0, 20);
    }
    gSeaweedSpawnX = 320;
}

void startGame()
{
    gScore = 0;
    gLives = 3;
    gChaos = 0;
    gFrameCount = 0;
    gDiverWX8 = 0;
    gDiverWY8 = 0;
    gTargetWY = 0;
    // 初始摄像机：让海面在 SURFACE_SCREEN_Y 处
    gCamWX = -(int)(SCREEN_WIDTH / 4);
    gCamWY = -(int)SURFACE_SCREEN_Y;
    initJellyfish();
    initTreasures();
    initBubbles();
    initSeaweed();
    gState = STATE_PLAY;
}

// =====================
// 更新摄像机（边界触发式跟随）
// =====================
void updateCamera()
{
    int dsx = w2sx(gDiverWX8 >> 3);
    int dsy = w2sy(gDiverWY8 >> 3);

    // 水平：角色靠近右边界才推摄像机右移，靠近左边界才拉回
    if (dsx > SCREEN_WIDTH - CAM_MARGIN_RIGHT)
    {
        gCamWX += dsx - (SCREEN_WIDTH - CAM_MARGIN_RIGHT);
    }
    else if (dsx < CAM_MARGIN_LEFT)
    {
        gCamWX -= CAM_MARGIN_LEFT - dsx;
    }

    // 垂直：带 1/2 缓动的边界跟随
    if (dsy < CAM_MARGIN_TOP)
    {
        gCamWY -= (CAM_MARGIN_TOP - dsy + 1) >> 1;
    }
    else if (dsy > GAME_H - CAM_MARGIN_BOTTOM)
    {
        gCamWY += (dsy - (GAME_H - CAM_MARGIN_BOTTOM) + 1) >> 1;
    }
}

// =====================
// 更新场景对象
// =====================
void updateJellyfish()
{
    for (int i = 0; i < MAX_JELLYFISH; i++)
    {
        if (!gJellyfish[i].active)
            continue;
        if (gFrameCount % 3 == (uint32_t)i % 3)
        {
            gJellyfish[i].wx += gJellyfish[i].vx >> 3;
            gJellyfish[i].wy += gJellyfish[i].vy >> 3;
        }
        int cL = gCamWX - 30, cR = gCamWX + SCREEN_WIDTH + 30;
        if (gJellyfish[i].wx < cL)
            gJellyfish[i].vx = abs(gJellyfish[i].vx);
        if (gJellyfish[i].wx > cR)
            gJellyfish[i].vx = -abs(gJellyfish[i].vx);
        if (gJellyfish[i].wy < 5)
            gJellyfish[i].vy = abs(gJellyfish[i].vy);
        if (gJellyfish[i].wy > WORLD_MAX_DEPTH - 5)
            gJellyfish[i].vy = -abs(gJellyfish[i].vy);
    }
}

void updateBubbles()
{
    int camL = gCamWX - 20;
    for (int i = 0; i < BUBBLE_COUNT; i++)
    {
        gBubbles[i].wy -= gBubbles[i].riseSpd;
        gBubbles[i].wx -= gBubbles[i].driftSpd;
        // 浮出水面或飘出屏幕左侧：在右侧重新生成
        if (gBubbles[i].wy < -10 || gBubbles[i].wx < camL)
        {
            gBubbles[i].wx = gCamWX + SCREEN_WIDTH + randRange(5, 50);
            gBubbles[i].wy = randRange(3, WORLD_MAX_DEPTH - 3);
            gBubbles[i].riseSpd = randRange(1, 3);
            gBubbles[i].driftSpd = randRange(1, 3);
        }
    }
}

void updateSeaweed()
{
    int cR = gCamWX + SCREEN_WIDTH;
    while (gSeaweedSpawnX < cR + 60)
    {
        // 复用屏幕外左侧的海草槽
        int oldest = 0;
        for (int i = 1; i < SEAWEED_COUNT; i++)
            if (gSeaweed[i].wx < gSeaweed[oldest].wx)
                oldest = i;
        gSeaweed[oldest].wx = gSeaweedSpawnX + randRange(0, 20);
        gSeaweed[oldest].wy = WORLD_MAX_DEPTH - randRange(0, 4);
        gSeaweed[oldest].h = randRange(6, 18);
        gSeaweed[oldest].phase = randRange(0, 20);
        gSeaweedSpawnX += randRange(16, 32);
    }
}

// =====================
// 绘图函数
// =====================

void drawDiver(int sx, int sy)
{
    int phase = (gFrameCount / 3) & 3;
    // 头盔
    display.drawCircle(sx + 2, sy, 4, SSD1306_WHITE);
    display.drawPixel(sx + 1, sy - 1, SSD1306_WHITE); // 面罩高光
    // 身体
    display.drawFastVLine(sx + 2, sy + 4, 4, SSD1306_WHITE);
    display.fillRect(sx - 1, sy + 4, 2, 3, SSD1306_WHITE); // 背包
    // 手臂
    display.drawPixel(sx + 5, sy + 5, SSD1306_WHITE);
    display.drawPixel(sx - 1, sy + 5, SSD1306_WHITE);
    // 脚蹼动画
    if (phase == 0 || phase == 2)
    {
        display.drawFastHLine(sx, sy + 8, 4, SSD1306_WHITE);
        display.drawFastHLine(sx + 1, sy + 9, 3, SSD1306_WHITE);
    }
    else if (phase == 1)
    {
        display.drawFastHLine(sx + 1, sy + 8, 4, SSD1306_WHITE);
        display.drawFastHLine(sx, sy + 9, 3, SSD1306_WHITE);
    }
    else
    {
        display.drawFastHLine(sx - 1, sy + 8, 4, SSD1306_WHITE);
        display.drawFastHLine(sx + 1, sy + 9, 3, SSD1306_WHITE);
    }
    // 呼出气泡
    if ((gFrameCount / 5) % 3 != 0)
        display.drawPixel(sx + 6, sy - 1, SSD1306_WHITE);
}

void drawJellyfish(int sx, int sy)
{
    int phase = (gFrameCount / 5) & 1;
    // 上半圆（钟形）
    for (int dx = -5; dx <= 5; dx++)
    {
        for (int dy = -5; dy <= 0; dy++)
        {
            if (dx * dx + dy * dy <= 25)
            {
                int px = sx + dx, py = sy + dy;
                if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < GAME_H)
                    display.drawPixel(px, py, SSD1306_WHITE);
            }
        }
    }
    // 清内部（空心）
    for (int dx = -3; dx <= 3; dx++)
        for (int dy = -3; dy <= -1; dy++)
        {
            int px = sx + dx, py = sy + dy;
            if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < GAME_H)
                display.drawPixel(px, py, SSD1306_BLACK);
        }
    // 底边线
    display.drawFastHLine(sx - 5, sy, 11, SSD1306_WHITE);
    // 内纹
    display.drawPixel(sx - 2, sy - 2, SSD1306_WHITE);
    display.drawPixel(sx + 2, sy - 2, SSD1306_WHITE);
    // 触须
    for (int t = 0; t < 5; t++)
    {
        int tx = sx - 4 + t * 2;
        int len = 3 + ((phase + t) & 1);
        if (tx >= 0 && tx < SCREEN_WIDTH)
            display.drawFastVLine(tx, sy + 1, len, SSD1306_WHITE);
    }
}

void drawTreasure(int sx, int sy, int tier)
{
    switch (tier % 3)
    {
    case 0: // 宝箱
        display.drawRect(sx - 4, sy - 3, 9, 7, SSD1306_WHITE);
        display.drawFastHLine(sx - 4, sy - 1, 9, SSD1306_WHITE);
        display.drawPixel(sx, sy, SSD1306_WHITE);
        break;
    case 1: // 宝珠
        display.drawCircle(sx, sy, 4, SSD1306_WHITE);
        display.drawPixel(sx - 1, sy - 1, SSD1306_WHITE);
        display.drawPixel(sx + 1, sy - 1, SSD1306_WHITE);
        break;
    case 2: // 皇冠
        display.drawFastHLine(sx - 4, sy + 2, 9, SSD1306_WHITE);
        display.drawFastVLine(sx - 4, sy - 1, 3, SSD1306_WHITE);
        display.drawFastVLine(sx, sy - 3, 5, SSD1306_WHITE);
        display.drawFastVLine(sx + 4, sy - 1, 3, SSD1306_WHITE);
        break;
    }
    // 闪烁
    if (((gFrameCount >> 3) + tier) % 3 == 0)
        display.drawPixel(sx + 5, sy - 4, SSD1306_WHITE);
}

void drawSeaweed(int sx, int sy, int h, int phase)
{
    // 快速整数摇摆（避免 sin 开销）
    int wobble = (int)(((gFrameCount + phase) >> 2) & 3) - 1; // -1,0,1,2,-1...
    for (int j = 0; j < h; j++)
    {
        int px = sx + (j > h / 2 ? wobble : 0);
        int py = sy - j;
        if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < GAME_H)
            display.drawPixel(px, py, SSD1306_WHITE);
    }
}

void drawSurface()
{
    int surfSY = w2sy(0);
    if (surfSY < -2 || surfSY > GAME_H + 2)
        return;

    // 波浪：用摄像机 X 偏移实现向左流动
    int offset = ((-gCamWX) >> 1) & 0x7; // 0~7
    for (int x = 0; x < SCREEN_WIDTH; x++)
    {
        int ph = (x + offset) & 0x7;
        int py = surfSY + (ph < 4 ? 0 : 1);
        if (py >= 0 && py < GAME_H)
            display.drawPixel(x, py, SSD1306_WHITE);
    }

    // 水面上方：稀疏点阵模拟天空/空气
    if (surfSY > 3)
    {
        for (int y = 0; y < surfSY - 1; y++)
        {
            if (y % 4 == 0)
            {
                for (int x = 2; x < SCREEN_WIDTH; x += 5)
                    display.drawPixel(x, y, SSD1306_WHITE);
            }
        }
    }
}

void drawSeabed()
{
    int bedSY = w2sy(WORLD_MAX_DEPTH);
    if (bedSY > GAME_H + 4 || bedSY < 0)
        return;
    for (int x = 0; x < SCREEN_WIDTH; x++)
    {
        int bx = (x + gCamWX) & 0xF;
        int bump = (bx < 8) ? (bx >> 1) : ((15 - bx) >> 1);
        int py = bedSY + bump;
        if (py >= 0 && py < SCREEN_HEIGHT)
            display.drawFastVLine(x, py, SCREEN_HEIGHT - py, SSD1306_WHITE);
    }
}

void drawDepthOverlay(int depthPct)
{
    // 越深，随机打黑点越多
    int darkLevel = map(depthPct, 0, 100, 0, 50);
    if (darkLevel < 4)
        return;
    uint32_t rng = (gFrameCount >> 3) * 1664525u + 1013904223u;
    for (int i = 0; i < darkLevel * 4; i++)
    {
        rng = rng * 1664525u + 1013904223u;
        int px = (rng >> 16) % SCREEN_WIDTH;
        rng = rng * 1664525u + 1013904223u;
        int py = (rng >> 16) % GAME_H;
        display.drawPixel(px, py, SSD1306_BLACK);
    }
}

void drawHUD(int depthPct, bool inChaos)
{
    display.drawFastHLine(0, GAME_H, SCREEN_WIDTH, SSD1306_WHITE);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    if (inChaos && (gFrameCount / 3) & 1)
    {
        display.setCursor(0, GAME_H + 1);
        display.print("!! CHAOS !!");
    }
    else
    {
        display.setCursor(0, GAME_H + 1);
        display.print("D:");
        display.print(depthPct);
        display.print("%  S:");
        display.print(gScore);
    }

    // 命数（小心形 5×4px）
    for (int i = 0; i < gLives; i++)
    {
        int hx = SCREEN_WIDTH - 7 - i * 8;
        int hy = GAME_H + 2;
        display.drawPixel(hx + 1, hy, SSD1306_WHITE);
        display.drawPixel(hx + 3, hy, SSD1306_WHITE);
        display.drawPixel(hx, hy + 1, SSD1306_WHITE);
        display.drawPixel(hx + 2, hy + 1, SSD1306_WHITE);
        display.drawPixel(hx + 4, hy + 1, SSD1306_WHITE);
        display.drawPixel(hx + 1, hy + 2, SSD1306_WHITE);
        display.drawPixel(hx + 3, hy + 2, SSD1306_WHITE);
        display.drawPixel(hx + 2, hy + 3, SSD1306_WHITE);
    }
}

// =====================
// 标题 / 结束画面
// =====================
void drawTitleScreen()
{
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);

    u8g2Fonts.setFont(u8g2_font_wqy12_t_gb2312);
    u8g2Fonts.drawUTF8(14, 14, "小小潜水员");
    display.setTextSize(1);
    display.setCursor(10, 17);
    display.print("DEEP SEA DIVER");

    drawJellyfish(105, 18);
    drawDiver(14, 32);

    u8g2Fonts.drawUTF8(0, 44, "入水:下潜  出水:上浮");
    display.setCursor(2, 48);
    display.print("Wet=dive  Dry=surface");

    if ((gFrameCount / 10) & 1)
        u8g2Fonts.drawUTF8(22, 62, "入水即开始");

    display.display();
}

void drawGameOverScreen()
{
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);

    u8g2Fonts.setFont(u8g2_font_wqy12_t_gb2312);
    u8g2Fonts.drawUTF8(22, 14, "游戏结束");
    display.setTextSize(1);
    display.setCursor(10, 22);
    display.print("Score: ");
    display.print(gScore);
    display.setCursor(10, 32);
    display.print("Best:  ");
    display.print(gHighScore);

    u8g2Fonts.drawUTF8(4, 50, "入水重新开始");
    if ((gFrameCount / 10) & 1)
    {
        display.setCursor(12, 56);
        display.print("Wet to retry");
    }
    display.display();
}

// =====================
// 主游戏循环
// =====================
void updateAndDrawGame()
{
    static const int tScores[MAX_TREASURES] = {10, 20, 30, 50, 80, 150};

    // ---- 传感器 ----
    int rawMoisture = readMoisture();
    int depthPct;
    if (gChaos > 0)
    {
        gChaos--;
        // 用帧号生成确定性噪声（无需 random，更快）
        int noise = (int)((gFrameCount * 73 + 19) & 0x3F) - 32;
        depthPct = constrain(moistureToDepthPct(rawMoisture) + noise, 0, 100);
    }
    else
    {
        depthPct = moistureToDepthPct(rawMoisture);
    }

    // ---- 目标深度映射（允许 -8 ~ MAX_DEPTH，-8 即浮出水面）----
    gTargetWY = map(depthPct, 0, 100, -8, WORLD_MAX_DEPTH);

    // ---- 潜水员移动 ----
    gDiverWX8 += DIVER_SPEED_X8;

    int diverWY = gDiverWY8 >> 3;
    int diff = gTargetWY - diverWY;
    // 惯性：75% 跟随（比除以6更快，响应更好）
    gDiverWY8 += (diff * 3) >> 2;

    int diverWX = gDiverWX8 >> 3;
    diverWY = gDiverWY8 >> 3;

    // ---- 摄像机 ----
    updateCamera();

    // ---- 更新场景 ----
    updateJellyfish();
    updateBubbles();
    updateSeaweed();

    // ---- 碰撞：水母 ----
    if (gChaos == 0)
    {
        for (int i = 0; i < MAX_JELLYFISH; i++)
        {
            if (!gJellyfish[i].active)
                continue;
            if (abs(gJellyfish[i].wx - diverWX) < 7 &&
                abs(gJellyfish[i].wy - diverWY) < 7)
            {
                gChaos = CHAOS_DURATION;
                gLives--;
                Serial.printf("碰到水母! 剩余:%d\n", gLives);
                if (gLives <= 0)
                {
                    if (gScore > gHighScore)
                        gHighScore = gScore;
                    gState = STATE_GAMEOVER;
                    return;
                }
                break;
            }
        }
    }

    // ---- 碰撞：宝藏 ----
    for (int i = 0; i < MAX_TREASURES; i++)
    {
        if (!gTreasures[i].active)
            continue;
        if (abs(gTreasures[i].wx - diverWX) < 10 &&
            abs(gTreasures[i].wy - diverWY) < 10)
        {
            gTreasures[i].active = false;
            gScore += tScores[gTreasures[i].tier];
            Serial.printf("宝藏! +%d\n", tScores[gTreasures[i].tier]);
        }
    }

    // ---- 补充宝藏 ----
    for (int i = 0; i < MAX_TREASURES; i++)
    {
        if (!gTreasures[i].active)
        {
            gTreasures[i].wx = gCamWX + SCREEN_WIDTH + randRange(50, 120);
            gTreasures[i].wy = randRange(5, WORLD_MAX_DEPTH - 5);
            gTreasures[i].active = true;
        }
    }

    // ---- 生存得分 ----
    if ((gFrameCount & 7) == 0)
        gScore += 1 + depthPct / 30;

    // =====================
    // 绘制（层次从底到顶）
    // =====================
    display.clearDisplay();

    // 1. 深度暗化叠加（最底层）
    drawDepthOverlay(depthPct);

    // 2. 海底
    drawSeabed();

    // 3. 海草
    for (int i = 0; i < SEAWEED_COUNT; i++)
    {
        int sx = w2sx(gSeaweed[i].wx);
        int sy = w2sy(gSeaweed[i].wy);
        if (sx >= -2 && sx < SCREEN_WIDTH + 2)
            drawSeaweed(sx, sy, gSeaweed[i].h, gSeaweed[i].phase);
    }

    // 4. 气泡（水面以下才绘制）
    for (int i = 0; i < BUBBLE_COUNT; i++)
    {
        if (gBubbles[i].wy <= 0)
            continue;
        int sx = w2sx(gBubbles[i].wx);
        int sy = w2sy(gBubbles[i].wy);
        if (sx >= 0 && sx < SCREEN_WIDTH && sy >= 0 && sy < GAME_H)
        {
            int sz = (gBubbles[i].riseSpd > 2) ? 2 : 1;
            display.drawCircle(sx, sy, sz, SSD1306_WHITE);
        }
    }

    // 5. 宝藏
    for (int i = 0; i < MAX_TREASURES; i++)
    {
        if (!gTreasures[i].active)
            continue;
        int sx = w2sx(gTreasures[i].wx);
        int sy = w2sy(gTreasures[i].wy);
        if (sx >= -6 && sx < SCREEN_WIDTH + 6 && sy >= -6 && sy < GAME_H + 6)
            drawTreasure(sx, sy, gTreasures[i].tier);
    }

    // 6. 水母
    for (int i = 0; i < MAX_JELLYFISH; i++)
    {
        if (!gJellyfish[i].active)
            continue;
        int sx = w2sx(gJellyfish[i].wx);
        int sy = w2sy(gJellyfish[i].wy);
        if (sx >= -8 && sx < SCREEN_WIDTH + 8 && sy >= -8 && sy < GAME_H + 8)
            drawJellyfish(sx, sy);
    }

    // 7. 海面（在水母/潜水员之上，模拟折射遮挡）
    drawSurface();

    // 8. 潜水员（最顶层）
    drawDiver(w2sx(diverWX), w2sy(diverWY));

    // 9. HUD
    drawHUD(depthPct, gChaos > 0);

    display.display();

    if ((gFrameCount & 0x1F) == 0)
        Serial.printf("[F%lu] 湿度:%d 深度:%d%% 位置:(%d,%d) Cam:(%d,%d) 分:%d\n",
                      gFrameCount, rawMoisture, depthPct,
                      diverWX, diverWY, gCamWX, gCamWY, gScore);
}

// =====================
// setup / loop
// =====================
void setup()
{
    Serial.begin(115200);
    // I2C 400kHz（快速模式，减少显示传输时间）
    Wire.begin(SDA_PIN, SCL_PIN, 400000);
    pinMode(DO_PIN, INPUT);
    pinMode(AO_PIN, INPUT);
    analogReadResolution(12);

    if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS))
    {
        Serial.println("SSD1306 启动失败");
        while (true)
            delay(100);
    }
    display.setTextWrap(false);

    u8g2Fonts.begin(display);
    u8g2Fonts.setFontMode(1);
    u8g2Fonts.setForegroundColor(SSD1306_WHITE);
    u8g2Fonts.setBackgroundColor(SSD1306_BLACK);

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(16, 28);
    display.print("DEEP SEA DIVER");
    display.display();
    delay(800);

    Serial.println("小小潜水员就绪");
    Serial.printf("传感器: DRY=%d WET=%d\n", SENSOR_DRY, SENSOR_WET);
}

void loop()
{
    unsigned long now = millis();
    if (now - gLastMs < (unsigned long)TARGET_FPS_MS)
    {
        delay(1);
        return;
    }
    gLastMs = now;
    gFrameCount++;

    switch (gState)
    {
    case STATE_TITLE:
    {
        drawTitleScreen();
        if (moistureToDepthPct(readMoisture()) > 30)
        {
            delay(300);
            startGame();
        }
        break;
    }
    case STATE_PLAY:
        updateAndDrawGame();
        break;
    case STATE_GAMEOVER:
    {
        drawGameOverScreen();
        if (moistureToDepthPct(readMoisture()) > 35)
        {
            delay(600);
            startGame();
        }
        break;
    }
    }
}