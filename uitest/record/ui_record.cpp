/*
 * Copyright (c) 2022 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <chrono>
#include <thread>
#include "external_calls.h"
#include "ui_record.h"

namespace OHOS::uitest {
    using namespace std;
    using namespace std::chrono;
    using nlohmann::json;

    static bool g_uiRecordRun = false;
    int g_callBackId = -1;
    static std::shared_ptr<InputEventCallback> g_uiCallBackInstance = nullptr;
    // enum TouchOpt : uint8_t {
    //     OP_CLICK, OP_LONG_CLICK, OP_DOUBLE_CLICK, OP_SWIPE, OP_DRAG, \
    //     OP_FLING, OP_HOME, OP_RECENT, OP_RETURN
    // };

    std::string g_operationType[9] = { "click", "longClick", "doubleClick", "swipe", "drag", \
                                       "fling", "home", "recent", "back" };

    const std::map <int32_t, TouchOpt> SPECIAL_KET_MAP = {
        {MMI::KeyEvent::KEYCODE_BACK, TouchOpt::OP_RETURN},
        {MMI::KeyEvent::KEYCODE_HOME, TouchOpt::OP_HOME},
    };

    static std::vector<SubscribeKeyevent> NEED_SUBSCRIBE_KEY = {
        {MMI::KeyEvent::KEYCODE_POWER, true, 0}, // 电源键按下订阅
        {MMI::KeyEvent::KEYCODE_POWER, false, 0}, // 电源键抬起订阅
        {MMI::KeyEvent::KEYCODE_VOLUME_UP, true, 0}, // 音量UP键按下订阅
        {MMI::KeyEvent::KEYCODE_VOLUME_DOWN, true, 0}, // 音量DOWN键按下订阅
        {MMI::KeyEvent::KEYCODE_ESCAPE, true, 0}, // esc键按下订阅
        {MMI::KeyEvent::KEYCODE_ESCAPE, false, 0}, // esc键抬起订阅
        {MMI::KeyEvent::KEYCODE_F1, true, 0}, // f1键按下订阅
        {MMI::KeyEvent::KEYCODE_F1, false, 0}, // f1键抬起订阅
        {MMI::KeyEvent::KEYCODE_ALT_LEFT, true, 0}, // alt-left键按下订阅
        {MMI::KeyEvent::KEYCODE_ALT_LEFT, false, 0}, // alt-left键抬起订阅
        {MMI::KeyEvent::KEYCODE_ALT_RIGHT, true, 0}, // alt-right键按下订阅
        {MMI::KeyEvent::KEYCODE_ALT_RIGHT, false, 0}, // alt-right键抬起订阅
        {MMI::KeyEvent::KEYCODE_FN, true, 0}, // fn键按下订阅
        {MMI::KeyEvent::KEYCODE_FN, false, 0}, // fn键抬起订阅
    };

    inline const std::string InputEventCallback::DEFAULT_DIR = "/data/local/tmp/layout";
    std::string EventData::defaultDir = InputEventCallback::DEFAULT_DIR;

    bool SpecialKeyMapExistKey(int32_t keyCode, TouchOpt &touchop)
    {
        auto iter = SPECIAL_KET_MAP.find(keyCode);
        if (iter!=SPECIAL_KET_MAP.end()) {
            touchop = iter->second;
            return true;
        }
        return false;
    }

    // KEY订阅模板函数
    void InputEventCallback::KeyEventSubscribeTemplate(SubscribeKeyevent& subscribeKeyevent)
    {
        std::set<int32_t> preKeys;
        std::shared_ptr<MMI::KeyOption> keyOption = std::make_shared<MMI::KeyOption>();
        keyOption->SetPreKeys(preKeys);
        keyOption->SetFinalKey(subscribeKeyevent.keyCode);
        keyOption->SetFinalKeyDown(subscribeKeyevent.isDown);
        keyOption->SetFinalKeyDownDuration(KEY_DOWN_DURATION);
        subscribeKeyevent.subId = MMI::InputManager::GetInstance()->SubscribeKeyEvent(keyOption,
            [this](std::shared_ptr<MMI::KeyEvent> keyEvent) {
            OnInputEvent(keyEvent);
        });
    }

    // key订阅
    void InputEventCallback::SubscribeMonitorInit()
    {
        for (size_t i = 0; i < NEED_SUBSCRIBE_KEY.size(); i++) {
            KeyEventSubscribeTemplate(NEED_SUBSCRIBE_KEY[i]);
        }
    }
    // key取消订阅
    void InputEventCallback::SubscribeMonitorCancel()
    {
        MMI::InputManager* inputManager = MMI::InputManager::GetInstance();
        if (inputManager == nullptr) {
            return;
        }
        for (size_t i = 0; i < NEED_SUBSCRIBE_KEY.size(); i++) {
            if (NEED_SUBSCRIBE_KEY[i].subId >= 0) {
                inputManager->UnsubscribeKeyEvent(NEED_SUBSCRIBE_KEY[i].subId);
            }
        }
    }

    void EventData::ReadEventLine()
    {
        std::ifstream inFile(defaultDir + "/" + "record.csv");
        std::string line;
        if (inFile.is_open())
        {
            while (std::getline(inFile, line)){
                std::cout << line << std::endl;
            }
            inFile.close();
        }
              // enum CaseTypes : uint8_t {
        //     OP_TYPE_ = 0, X_POSI, Y_POSI, X2_POSI, Y2_POSI, INTERVAL, LENGTH, VELO, \
        //     MAX_VEL, W_ID, W_TYPE, W_TEXT, W2_ID, W2_TYPE, W2_TEXT, BUNDLE, ABILITY
        // };
        // char buffer[100];
        // while (!inFile.eof()) {
        //     inFile >> buffer;
        //     std::string delim = ",";
        //     auto caseInfo = TestUtils::split(buffer, delim);
        //     if (inFile.fail()) {
        //         break;
        //     } else {
        //         std::cout << caseInfo[OP_TYPE_] << ";"
        //                 << std::stoi(caseInfo[X_POSI]) << ";"
        //                 << std::stoi(caseInfo[Y_POSI]) << ";"
        //                 << std::stoi(caseInfo[X2_POSI]) << ";"
        //                 << std::stoi(caseInfo[Y2_POSI]) << ";"
        //                 << std::stoi(caseInfo[INTERVAL]) << ";"
        //                 << std::stoi(caseInfo[LENGTH]) << ";"
		// 				<< std::stoi(caseInfo[VELO]) << ";"
		// 				<< std::stoi(caseInfo[MAX_VEL]) << ";"
        //                 << caseInfo[W_ID] << ";"
        //                 << caseInfo[W_TYPE] << ";"
        //                 << caseInfo[W_TEXT] << ";"
        //                 << caseInfo[W2_ID] << ";"
        //                 << caseInfo[W2_TYPE] << ";"
        //                 << caseInfo[W2_TEXT] << ";"
        //                 << caseInfo[BUNDLE] << ";"
        //                 << caseInfo[ABILITY] << ";"
		// 				<< std::endl;
        //     }
        //     int gTimeIndex = 1000;
        //     usleep(std::stoi(caseInfo[INTERVAL]) * gTimeIndex);
        // }
    }

    // KEY_ACTION
    void InputEventCallback::OnInputEvent(std::shared_ptr<MMI::KeyEvent> keyEvent) const
    {
        TouchOpt touchOpt;
        // std::cout << " @@@@@ keyevent: " << keyEvent->GetKeyCode() << " , " << keyEvent->GetKeyAction() << std::endl;
        if (SpecialKeyMapExistKey(keyEvent->GetKeyCode(), touchOpt)) {
            if (keyEvent->GetKeyAction() == MMI::KeyEvent::KEY_ACTION_DOWN) {
                return;
            } else if (keyEvent->GetKeyAction() == MMI::KeyEvent::KEY_ACTION_UP) {
                PointerInfo& info = pointerTracker_.GetSnapshootPointerInfo();
                info.SetTouchOpt(touchOpt);
                findWidgetsAllow_ = true;
                widgetsCon.notify_all();
                // g_isSpecialclick = false;
                pointerTracker_.SetLastClickInTracker(false);
                return;
            }
        }
        touchTime = GetCurrentMillisecond();
        auto item = keyEvent->GetKeyItem(keyEvent->GetKeyCode());
        if (!item) {
            std::cout << "GetPointerItem Fail" << std::endl;
        }
        KeyEventInfo info;
        info.SetActionTime(keyEvent->GetActionTime());
        info.SetKeyCode(keyEvent->GetKeyCode());
        if (keyEvent->GetKeyAction() == MMI::KeyEvent::KEY_ACTION_DOWN) {
            // 三键以上的同时按键无效
            if (keyeventTracker_.GetInfos().size() >= KeyeventTracker::MAX_COMBINATION_SIZE) {
                LOG_I("More than three keys are invalid at the same time");
                std::cout << "More than three keys are invalid at the same time" << std::endl;
                return;
            }
            if (KeyeventTracker::isCombinationKey(info.GetKeyCode())) {
                keyeventTracker_.SetNeedRecord(true);
                keyeventTracker_.AddDownKeyEvent(info);
            } else {
                keyeventTracker_.SetNeedRecord(false);
                KeyeventTracker snapshootKeyTracker = keyeventTracker_.GetSnapshootKey(info);
                 // cout打印 + record.csv保存
                snapshootKeyTracker.WriteSingleData(info, g_cout_lock);
                auto json = snapshootKeyTracker.WriteSingleData(info, outFile, g_csv_lock);
                if (abcCallBack != nullptr){
                    auto data = nlohmann::json();
                    data["code"] = "3";
                    data["data"] = json;
                    abcCallBack(data);
                }
            }
        } else if (keyEvent->GetKeyAction() == MMI::KeyEvent::KEY_ACTION_UP) {
            if (!KeyeventTracker::isCombinationKey(info.GetKeyCode())) {
                return;
            }
            if (keyeventTracker_.IsNeedRecord()) {
                keyeventTracker_.SetNeedRecord(false);
                KeyeventTracker snapshootKeyTracker = keyeventTracker_.GetSnapshootKey(info);
                // cout打印 + record.csv保存json
                snapshootKeyTracker.WriteCombinationData(g_cout_lock);
                auto json = snapshootKeyTracker.WriteCombinationData(outFile, g_csv_lock);
                if (abcCallBack != nullptr){
                    auto data = nlohmann::json();
                    data["code"] = "3";
                    data["data"] = json;
                    abcCallBack(data);
                }
            }
            keyeventTracker_.AddUpKeyEvent(info);
        }
    }

    // AXIS_ACTION
    void InputEventCallback::OnInputEvent(std::shared_ptr<MMI::AxisEvent> axisEvent) const {
    }

    void InputEventCallback::TimerReprintClickFunction ()
    {
        while (g_uiRecordRun) {
            std::unique_lock <std::mutex> clickLck(g_clickMut);
            while (!isLastClick_) {
                clickCon.wait(clickLck);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds((int)( PointerTracker::INTERVAL_THRESHOLD * gTimeIndex)));
            if (isLastClick_) {
                isLastClick_ = false;
                pointerTracker_.SetLastClickInTracker(false);
                findWidgetsAllow_ = true;
                widgetsCon.notify_all();
            }
        }
    }

    void InputEventCallback::TimerTouchCheckFunction()
    {
        while (g_uiRecordRun) {
            unique_lock<mutex> lock(timerMut);
            timerCon.wait_for(lock,std::chrono::milliseconds(TIMEINTERVAL), [this]{ return stopFlag; });
            int currentTime = GetCurrentMillisecond();
            int diff = currentTime - touchTime;
            if (diff >= TIMEINTERVAL) {
                std::cout << "No operation detected for 5 seconds, press ctrl + c to save this file?" << std::endl;
            }
        }
    }

    void InputEventCallback::FindWidgetsandWriteData()
    {
        while (g_uiRecordRun){
            std::unique_lock<std::mutex> widgetsLck(widgetsMut);
            while (!findWidgetsAllow_) {
                widgetsCon.wait(widgetsLck);
            }
            if(!g_uiRecordRun){
                return;
            }
            if (abcCallBack != nullptr){
                auto data = nlohmann::json();
                data["code"] = "2";
                abcCallBack(data);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(gTimeIndex)); // 确保界面已更新
            ApiCallErr err(NO_ERROR);
            driver.FindWidgets(selector, rev, err, true);
            PointerInfo& info = pointerTracker_.GetSnapshootPointerInfo();
            pointerTracker_.WriteData(info, g_cout_lock);
            auto json = pointerTracker_.WriteData(info, outFile, g_csv_lock);
            if (abcCallBack != nullptr){
                auto data = nlohmann::json();
                data["code"] = "3";
                data["data"] = json;
                abcCallBack(data);
            }
            findWidgetsAllow_ = false;
            widgetsCon.notify_all();
        }
    }


    void InputEventCallback::OnInputEvent(std::shared_ptr<MMI::PointerEvent> pointerEvent) const
    {
        MMI::PointerEvent::PointerItem item;
        bool result = pointerEvent->GetPointerItem(pointerEvent->GetPointerId(), item);
        if (!result) {
            std::cout << "GetPointerItem Fail" << std::endl;
        }
        touchTime = GetCurrentMillisecond();
        TouchEventInfo touchEvent {};
        touchEvent.actionTime = pointerEvent->GetActionTime();
        touchEvent.downTime = item.GetDownTime();
        touchEvent.x = item.GetDisplayX();
        touchEvent.y = item.GetDisplayY();
        touchEvent.wx = item.GetWindowX();
        touchEvent.wy = item.GetWindowY();
        std::chrono::duration<double>  duration = touchEvent.GetActionTimeStamp() - touchEvent.GetDownTimeStamp();
        touchEvent.durationSeconds = duration.count();
        // std::cout << " @@@@@ touchEvent: " << pointerEvent->GetPointerAction() << " , "
        //           << touchEvent.actionTime << " , " << touchEvent.downTime << std::endl;
        if (pointerEvent->GetPointerAction() == MMI::PointerEvent::POINTER_ACTION_DOWN) {
            std::unique_lock<std::mutex> widgetsLck(widgetsMut);
            while (findWidgetsAllow_) {
                widgetsCon.wait(widgetsLck);
            }
            if (recordMode != "point") {
                touchEvent.attributes = FindWidget(driver, touchEvent.x, touchEvent.y).GetAttrMap();
            }
            pointerTracker_.HandleDownEvent(touchEvent);
        } else if (pointerEvent->GetPointerAction() == MMI::PointerEvent::POINTER_ACTION_MOVE) {
            pointerTracker_.HandleMoveEvent(touchEvent);
        } else if (pointerEvent->GetPointerAction() == MMI::PointerEvent::POINTER_ACTION_UP) {
            if (recordMode != "point") {
                touchEvent.attributes = FindWidget(driver, touchEvent.x, touchEvent.y).GetAttrMap();
            }
            pointerTracker_.HandleUpEvent(touchEvent);
            if (pointerTracker_.IsNeedWrite()) {
                PointerInfo info = pointerTracker_.GetSnapshootPointerInfo();
                if (info.GetTouchOpt() != OP_CLICK){
                    isLastClick_ = false;
                    findWidgetsAllow_ = true;
                    widgetsCon.notify_all();
                }
                if (info.GetTouchOpt() == OP_CLICK){
                    isLastClick_ = true;
                    clickCon.notify_all();
                }
                pointerTracker_.SetNeedWrite(false);
            }
        }
    }
    
    bool InputEventCallback::InitReportFolder()
    {
        if (opendir(DEFAULT_DIR.c_str()) == nullptr) {
            int ret = mkdir(DEFAULT_DIR.c_str(), S_IROTH | S_IRWXU | S_IRWXG);
            if (ret != 0) {
                std::cerr << "failed to create dir: " << DEFAULT_DIR << std::endl;
                return false;
            }
        }
        return true;
    }

    bool InputEventCallback::InitEventRecordFile()
    {
        if (!InitReportFolder()) {
            return false;
        }
        filePath = DEFAULT_DIR + "/" + "record.csv";
        outFile.open(filePath, std::ios_base::out | std::ios_base::trunc);
        if (!outFile) {
            std::cerr << "Failed to create csv file at:" << filePath << std::endl;
            return false;
        }
        std::cout << "The result will be written in csv file at location: " << filePath << std::endl;
        return true;
    }
    void InputEventCallback::RecordInitEnv(const std::string &modeOpt)
    {
        recordMode = modeOpt;
        ApiCallErr err(NO_ERROR);
        driver.FindWidgets(selector, rev, err, true);
        auto screenSize = driver.GetDisplaySize(err);
        Rect windowBounds = Rect(0, screenSize.px_, 0,  screenSize.py_);
        std::cout<< "windowBounds : (" << windowBounds.left_ << ","
                << windowBounds.top_ << "," << windowBounds.right_ << ","
                << windowBounds.bottom_ << ")" << std::endl;
        pointerTracker_.SetWindow(windowBounds);
        std::vector<std::string> names = GetForeAbility();
        std::cout << "Current ForAbility :" << names[ZERO] << ", " << names[ONE] << std::endl;
        if (outFile.is_open()) {
            outFile << "windowBounds" << ',';
            outFile << windowBounds.left_ << ',';
            outFile << windowBounds.top_ << ',';
            outFile << windowBounds.right_ << ',';
            outFile << windowBounds.bottom_ << ',';
            outFile << "0,0,0,0,,,,,,,";
            outFile << names[ZERO] << ',';
            outFile << names[ONE] << ',' << std::endl;
        }
    }

    int32_t UiDriverRecordStart(std::string modeOpt)
    {
        g_uiCallBackInstance = std::make_shared<InputEventCallback>();
        return UiDriverRecordStartTemplate(modeOpt);
    }

    int32_t UiDriverRecordStart(std::function<void(nlohmann::json)> healder,  std::string modeOpt)
    {
        g_uiCallBackInstance = std::make_shared<InputEventCallback>();
        g_uiCallBackInstance->SetAbcCallBack(healder);
        return UiDriverRecordStartTemplate(modeOpt);
    }

    int32_t UiDriverRecordStartTemplate(std::string modeOpt)
    {
        auto abcCallBack = g_uiCallBackInstance->GetAbcCallBack();
        if (abcCallBack != nullptr){
            auto data = nlohmann::json();
            data["code"] = "0";
            abcCallBack(data);
        }
        g_uiCallBackInstance->RecordInitEnv(modeOpt);
        if (!g_uiCallBackInstance->InitEventRecordFile()) {
            return OHOS::ERR_INVALID_VALUE;
        }
        if (g_uiCallBackInstance == nullptr) {
            std::cout << "nullptr" << std::endl;
            return OHOS::ERR_INVALID_VALUE;
        }
        // 按键订阅
        g_uiCallBackInstance->SubscribeMonitorInit();
        g_callBackId = MMI::InputManager::GetInstance()->AddMonitor(g_uiCallBackInstance);
        if (g_callBackId == -1) {
            std::cout << "Startup Failed!" << std::endl;
            return OHOS::ERR_INVALID_VALUE;
        }
        g_uiRecordRun = true;
        // 补充click打印线程
        std::thread clickThread(&InputEventCallback::TimerReprintClickFunction, g_uiCallBackInstance);
        // touch计时线程
        std::thread toughTimerThread(&InputEventCallback::TimerTouchCheckFunction, g_uiCallBackInstance);
        // widget&data 线程
        std::thread dataThread(&InputEventCallback::FindWidgetsandWriteData, g_uiCallBackInstance);
        std::cout << "Started Recording Successfully..." << std::endl;
        int flag = getc(stdin);
        std::cout << flag << std::endl;
        if (abcCallBack != nullptr){
            auto data = nlohmann::json();
            data["code"] = "1";
            abcCallBack(data);
        }
        clickThread.join();
        toughTimerThread.join();
        dataThread.join();
        return OHOS::ERR_OK;
    }

    void UiDriverRecordStop()
    {
        g_uiRecordRun = false;
        if(g_uiCallBackInstance != nullptr){
            g_uiCallBackInstance->isLastClick_ = true;
            g_uiCallBackInstance->findWidgetsAllow_ = true;
            g_uiCallBackInstance->stopFlag = true;
            g_uiCallBackInstance->widgetsCon.notify_all();
            g_uiCallBackInstance->clickCon.notify_all();
            g_uiCallBackInstance->timerCon.notify_all();
            g_uiCallBackInstance->SubscribeMonitorCancel();
            MMI::InputManager::GetInstance()->RemoveMonitor(g_callBackId);
            g_callBackId = -1;
            g_uiCallBackInstance = nullptr;
        }
    }
} // namespace OHOS::uitest