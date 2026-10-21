#include "Game/Language.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <string>
#include <unordered_map>

namespace
{
ELanguage CurrentLanguage = ELanguage::English;

struct FTranslation
{
    std::string_view English;
    // UTF-8 here; Localize gives it as Windows-1251.
    std::string_view Russian;
};

// The pairs of LanguageGet's branches in Vyruz.exe, the tables of DifficultyName (00403310), its help texts (0050d3e8) and the finale's lines (0050d740),
// and the titles MenuSetLanguageStrings (004227c0) sets. Where the Russian build left a text in English or French for its second language, that text is kept.
constexpr FTranslation Translations[] = {
    { "%d saved session%s available.", "%d сессий%s доступно%s." },
    { "%d session%s saved.", "%d сессий%s сохран%s." },
    { "%s   (%d:%d:%d)\nEnergy : %d\nShield : %d\nDifficulty : %s ...\n", "%s   (%d:%d:%d)\nЭнергия  : %d\nЩит : %d\nСложность : %s ...\n" },
    { "%s,\n   you need MORE Cash\n     to see the Map !", "%s,\n   нужно БОЛЬШЕ Денег\n     для просмотра Карты !" },
    { "... DISCONNECTED ...", "... ОТСОЕДИНЕНИЕ ..." },
    { "... GENERAL NETWORK FAILURE ...", "... ОШИБКА СЕТИ ..." },
    { "... SYSTEM HALTED ...", "... ОШИБКА СИСТЕМЫ ..." },
    { "/Audio", "/Аудио" },
    { "/Audio/FX", "/Аудио/Эффекты" },
    { "/Audio/Music", "/Аудио/Музыка" },
    { "/Audio/Voices", "/Аудио/Гол." },
    { "/Control", "/Управление" },
    { "/Details", "/Детали" },
    { "/Device", "Устройство" },
    { "/Difficulty", "/Сложность" },
    { "/Driver", "/Пилот" },
    { "/FX,Voices", "/Эфф, Голос" },
    { "/Language", "/Язык" },
    { "/Music", "/Музыка" },
    { "/Name", "/Имя" },
    { "/Select", "/Выбор" },
    { "/Session", "/Сессия" },
    { "/Session/Name", "/Сессия/Имя" },
    { "/Settings", "/Настройки" },
    { "/Video", "/Видео" },
    { "/Video/Details", "/Видео/Детали" },
    { "A taste of Hell.", "Вкус Ада." },
    { "AMMO        %s", "БОЕЗАПАС    %s" },
    { "AMMUNITIONS", "АММУНИЦИЯ" },
    { "Add new name", "Добавить новое имя" },
    { "Add, delete or select player name.", "Добавить, удалить или выбрать имя игрока." },
    { "All Weapons Available !!!", "Все Оружие Доступно !" },
    { "Ammo", "Аммуниция" },
    { "Audio", "Аудио" },
    { "Auto Shield Up", "Авто восстановление" },
    { "Auto-playing game demo.", "Автоматическое демо игры." },
    { "Back", "Назад" },
    { "Backward", "Назад" },
    { "Blocked Motor !!!", "Мотор Заглох !!" },
    { "COORDINATES (X/Y)", "КООРДИНАТЫ (X/Y)" },
    { "CURRENT WEAPON", "ТЕКУЩЕЕ ОРУЖИЕ" },
    { "Cannot add more name.", "Нельзя добавить еще имя." },
    { "Cannot change this setting during the game.", "Невозможно изменить эту настройку во время игры." },
    { "Cannot open gates while invisible !", "Невозможно открыть вороты невид. !" },
    { "Cannot take lift while invisible !", "Нельзя вызвать лифт при невидимости !" },
    { "Channels : Mono", "Каналы : Моно" },
    { "Channels : Stereo", "Каналы : Стерео" },
    { "Circular Fire !", "Круговой Огонь !" },
    { "Code #%d : part %d !", "Код #%d : часть %d !" },
    { "Code #%d complete !", "Код #%d завершен !" },
    { "Code #%d regenerated !", "Ключ #%d регенер. !" },
    { "Control", "Управление" },
    { "Credits", "Авторы" },
    { "Current Graphic Details : ", "Текущая Граф. Детализация: " },
    { "Current language is %s.", "Текущий язык : %s." },
    { "Current name : %s", "Текщее имя : %s" },
    { "Current voice type is %s.", "Текущий тип голоса : %s." },
    { "Custom", "Собственная" },
    { "DAMAGE      %d", "ПОВРЕЖДЕНИЕ %d" },
    { "DAMAGE-AREA %d", "ОБЛАСТЬ ПОВР. %d" },
    { "DANGER : SHIELD IS LOW", "ОПАСНО : ЩИТ НА НУЛЕ" },
    { "DANGER WARNING", "ПРЕДУПРЕЖДЕНИЕ" },
    { "DESTROYED ENEMIES", "УНИЧТОЖЕНО ВРАГОВ" },
    { "Delete '%s' saved sessions ?", "Удалить сохраненные сессии '%s' ?" },
    { "Delete name", "Удалить имя" },
    { "Demo", "Демо" },
    { "Destroy Around !!!", "Уничтожь Все Вокруг !!" },
    { "Details Options", "Опции Детализации" },
    { "Details, Device, Video Mode, Gamma.", "Детали, Устройство, Видеорежим, Гамма." },
    { "Difficulty : %s", "Сложность : %s" },
    { "Direction : ABSOLUTE", "Направление : АБСОЛЮТН." },
    { "Direction : RELATIVE", "Направление : ОТНОСИТ." },
    { "ELAPSED TIME", "ВРЕМЕНИ ПРОШЛО" },
    { "ENEMY %d", "ВРАГОВ %d" },
    { "ENERGY", "ЭНЕРГИЯ" },
    { "Easy", "Легко" },
    { "Edit your string.", "Редактировать текст" },
    { "End Game", "Завершить Игру" },
    { "End this Game ?", "Закончить Игру ?" },
    { "Energy +%d", "Энергия +%d" },
    { "Energy : %d\nShield : %d\nDifficulty : %s ...\n", "Энергия  : %d\nЩит : %d\nСложность : %s ...\n" },
    { "English", "Английс" },
    { "English.", "Русский." },
    { "Exit", "Выход" },
    { "Exit Now ?", "Выйти Сейчас ?" },
    { "Extras", "Дополнения" },
    { "FIRE-DELAY  %d", "ЗАДЕРЖКА-ОГНЯ %d" },
    { "FIRE-RANGE  %d", "ДИАПАЗОН-ОГНЯ  %d" },
    { "Fire", "Ог." },
    { "For Beginners Only !!!", "Только Для Новичков !!!" },
    { "Forward", "Вперед" },
    { "Frequency : %d Hz", "Частота : %d Гц" },
    { "Gamma FX", "Гамма Эфф." },
    { "Gamma value : ", "Величина Гаммы : " },
    { "Gate needs full code #%d !", "Воротам нужен полный код #%d !" },
    { "Gate needs key #%d !", "Воротам нужен ключ #%d !" },
    { "Ground/Smoke details", "Детали Земли/Дыма" },
    { "HIGH SPEED", "ВЫСОК. СКОРОСТЬ" },
    { "HUD Green Info", "Info (affichage vert)" },
    { "Hard", "Тяжело" },
    { "Help", "Помощь" },
    { "High", "Высокая" },
    { "High.", "Высокая" },
    { "High/Low Speed", "Высокая/Низкая Скорость" },
    { "Hit~", "Удар %" },
    { "Hurt me !", "Жестокость !" },
    { "Input device, Key settings.", "Устройство ввода, Настройки клавиатуры." },
    { "Insane", "Безумно" },
    { "Install Indeo 5.11 or higher to see the intro.", "Установите Indeo 5.11 или выше для просмотра интро." },
    { "Intro", "Интро" },
    { "Intro animation file is not present.", "Интро фильм не найден." },
    { "Inventory", "Инвентарь" },
    { "Invisible Drone !", "Невидимый Дрон !" },
    { "Invulnerability !", "Неуязвимость !" },
    { "KILLED %d", "УБИТ %d" },
    { "Key #%d", "Ключ #%d" },
    { "Key #%d !", "Ключ #%d" },
    { "Key : ", "Ключ : " },
    { "Key : %s", "Ключ : %s" },
    { "Key settings", "Настройки клавиатуры" },
    { "Killed", "Убито" },
    { "LOW SPEED", "НИЗК. СКОРОСТЬ" },
    { "Language", "Язык" },
    { "Left", "Влево" },
    { "Level", "Уровень" },
    { "Licensed to %s", "Лицензировано для  %s" },
    { "Lift needs full code !", "Лифту нужен полный код !" },
    { "Lift needs key #%d !", "Лифту нужен ключ #%d !" },
    { "Load Game", "Загрузить Игру" },
    { "Low", "Низкая" },
    { "Low.", "Низкая." },
    { "MONEY", "ДЕНЬГИ" },
    { "Map", "Карта" },
    { "Modify setting.", "Изменить настройки." },
    { "Money", "Деньги" },
    { "Money +%d", "Деньги +%d" },
    { "Music", "Музыка" },
    { "Music : OFF", "Музыка : ВЫКЛ" },
    { "Music : ON", "Музыка : ВКЛ" },
    { "Music is desactivated.", "Музыка выключена." },
    { "Music, Sound FX, Quality, Frequency, Mono/Stereo.", "Музыка, Эффекты, Качество, Частота, Моно/Стерео." },
    { "NOTE : ONLY MINES CAN BE USED WHILE INVISIBLE", "ЗАМЕЧАНИЕ : ПРИ НЕВИДИМОСТИ ИСПОЛЬЗУЮТСЯ ТОЛЬКО МИНЫ" },
    { "Name", "Имя" },
    { "Name : %s", "Имя : %s" },
    { "New name : ", "Новое имя : " },
    { "Next Weapon", "След. Оружие" },
    { "No", "Нет" },
    { "No !", "Нет !" },
    { "No Inertia Control !", "Нет Контроля Инерции !" },
    { "No Light !!!", "Нет Света !!!" },
    { "No Saving.", "Нет Сохранения." },
    { "No key.", "Нет ключа." },
    { "No name to select.", "Нет имени для выбора." },
    { "No saved session.", "Нет сохраненной сессии." },
    { "No saved sessions.", "Нет сохраненных сессий." },
    { "No thanks.", "Нет, хорош" },
    { "Normal", "Норма" },
    { "Not available.", "Не доступно." },
    { "Not enough Cash %s !\n You need %d $\n  to save this game ...", "Недостаточно Денег %s !\n Нужно %d $\n  для сохранения игры ..." },
    { "Not yet implemented.", "Еще не выполнено." },
    { "OFF", "ВЫКЛ" },
    { "ON", "ВКЛ" },
    { "Options", "Опции" },
    { "Other settings ...", "Другие настройки ..." },
    { "Panel Config.", "Конфигурация Панели" },
    { "Panel Help", "Помощь Панели" },
    { "Pinball Effect !", "Эффект Пинболл!" },
    { "Press ENTER to validate : ", "Нажмите ВВОД для закрепления : " },
    { "Quality : %d bits", "Качество : %d бит" },
    { "RADAR (RED: ENEMY, YELLOW: KEY/CODE, BLUE: LIFT)", "РАДАР (КРАСНЫЙ: ВРАГ, ЖЕЛТЫЙ: КЛЮЧ/КОД, СИНИЙ: ЛИФТ)" },
    { "REMAINING ENEMIES", "ОСТАЛОСЬ ВРАГОВ" },
    { "Rank", "Ранг" },
    { "Reactor FX", "Эфф. Реактора" },
    { "Registration key", "Ключ регистрации" },
    { "Restart Level", "Начать Уровень Заново" },
    { "Restore default key settings ?", "Восстановить настройки по умолчанию ?" },
    { "Restore default settings", "Восстановить по умолч." },
    { "Resume Game", "Продолжить Игру" },
    { "Retry Level", "Повторить уровень" },
    { "Retry level ?", "Повторить ?" },
    { "Reversed Commands !", "Команды Резервир. !" },
    { "Right", "Вправо" },
    { "Russian", "Русский" },
    { "SHIELD", "ЩИТ" },
    { "SHIELD : %d / 100      ENERGY : %d / 100      AMMO : %d      CASH : %d $", "ЩИТ : %d / 100      ЭНЕРГИЯ : %d / 100      АММУН. : %d      ДЕНЬГИ : %d $" },
    { "Save your game for %d $", "Sauvez votre partie pour %d $" },
    { "Saved sessions", "Сохр. сессии" },
    { "Saving Completed.", "Сохранение Заверш." },
    { "Saving in %d second%s", "Сохранение за %d секунд%s" },
    { "Scores", "Рекорды" },
    { "Scores, Credits, Intro, Slide Show, Demo.", "Очки, Авторы, Интро, Слайд-Шоу, Демо." },
    { "Select player name", "Выберите имя игрока" },
    { "Select to activate Music.", "Включение музыки." },
    { "Select to activate Sound FX.", "Включение звуковых эффектов" },
    { "Select to activate Voices.", "Включение голосов." },
    { "Select to change difficulty level.", "Выберите для изменения сложности." },
    { "Select to desactivate Music.", "Отключение музыки." },
    { "Select to play with Joypad.", "Выберите для игры с Джойпада." },
    { "Select to play with Keyboard.", "Выберите для игры с Клавиатуры." },
    { "Select to play with Mouse.", "Выберите для игры с Мышью." },
    { "Select to remove Sound FX.", "Удаление звуковых эффектов." },
    { "Select to remove Voices.", "Удаление голосов." },
    { "Select to view intro animation.", "Выберите для просмотра интро фильма." },
    { "Shadow", "Тень" },
    { "Shield +%d", "Щит +%d" },
    { "Slide Show", "Слайд-Шоу" },
    { "Sound FX : OFF", "Эффекты : ВЫКЛ" },
    { "Sound FX : ON", "Эффекты : ВКЛ" },
    { "Sound FX are removed.", "Эффекты удалены." },
    { "Sound FX, Voices", "Эффекты, Голоса" },
    { "Sound driver (DirectSound) is unavalaible.", "Звуковой драйвер (DirectSound) не доступен." },
    { "Start New Game", "Новая Игра" },
    { "Strafe Left", "Стрейф Влево" },
    { "Strafe Right", "Стрейф Вправо" },
    { "THE BRAVEST RESISTANCE FIGHTERS", "ЛУЧШИЕ БОЙЦЫ СОПРОТИВЛЕНИЯ" },
    { "TV Borders", "Границы ТВ" },
    { "Thrust loss !!!", "Удар потерян!!!" },
    { "Time", "Время" },
    { "Time Warp !", "Ускорение Времени !" },
    { "Touch & Destroy !", "Коснись & Уничтожь !" },
    { "Tower", "Башня" },
    { "Translucent Panel", "Прозрачная Панель" },
    { "Trap !!!", "Ловушка !!!" },
    { "Triple Fire !!!", "Тройной Огонь !" },
    { "Tune Music Volume", "Громкость Музыки" },
    { "Tune Sound FX Volume", "Грокость эффектов" },
    { "Tune Voices Volume", "Громк. голоса" },
    { "Turning left or right is relative to the Vyruz direction.", "Повороты влево или вправо относительно направления Вируза." },
    { "Unknown soldier", "Неизв. солдат" },
    { "Use JoyPad", "Использовать ДжойПад" },
    { "Use Keyboard", "Исп. Клав." },
    { "Use Mouse", "Исп. Мышь" },
    { "User name, Game difficulty level.", "Имя игрока, Уровень сложности." },
    { "User settings", "Настройки Игрока" },
    { "Using Code #%d !", "Activation Code #%d !" },
    { "Using Key #%d !", "Исп. Ключ #%d !" },
    { "Value : %d %%", "Громк. : %d %%" },
    { "Video", "Видео" },
    { "Video, Audio, Control, User settings, Language", "Видео, Аудио, Управление, Настройки Игрока, Язык" },
    { "Video, Audio, Control, User settings, Language.", "Видео, Аудио, Управление, Настройки, Язык." },
    { "Voice type", "Тип голоса" },
    { "Voices : OFF", "Голос: ВЫКЛ" },
    { "Voices : ON", "Голос: ВКЛ" },
    { "Voices are removed.", "Голоса удалены." },
    { "WARNING : YOU ARE LOCKED", "ВНИМАНИЕ : ВЫ НА ПРИЦЕЛЕ" },
    { "WEAPON      %s", "ОРУЖИЕ      %s" },
    { "Wait ...", "Подождите ..." },
    { "Wait, lift will come.", "Подождите, лифт идет." },
    { "Waiting key or button event ...", "Ожидание нажатия клавиши или кнопки ..." },
    { "Weapon #1", "Оруж #1" },
    { "Weapon #2", "Оруж #2" },
    { "Weapon #3", "Оруж #3" },
    { "Weapon #4", "Оруж #4" },
    { "Weapon #5", "Оруж #5" },
    { "Weapon #6", "Оруж #6" },
    { "Weapon #7", "Оруж #7" },
    { "Weapon #8", "Оруж #8" },
    { "Weapon #9", "Оруж #9" },
    { "Weapon Jam !", "Осечка !" },
    { "Well done %s !!!\nFast Kill Bonus +%d $", "Отлично %s !!!\nБонус за Расправу +%d $" },
    { "Yes", "Да" },
    { "Yes please !", "Да, пожалуйста !" },
    { "Yes, bye.", "Да, досвидания." },
    { "You are so professional %s !\nNo more enemy in this level !\n  Real Killer Bonus : +%d $", "Какой профессионал %s !\nНа этом уровне больше нет врагов !\n  Бонус за Убийтсво : +%d $" },
    { "Your graphic card doesn't support gamma correction.", "Ваша графическая карта не поддерживает коррекцию гаммы." },
    { "[Next ...]", "[Вперед...]" },
    { "and", "и" },
    { "key", "жмите" },
    // The key and mouse names of the tables at 0051828c and 00517908, which LanguageSet (00401000) switches with the language.
    { "ESCAPE", "ECHAP" },
    { "NUM LOCK", "VERR-NUM" },
    { "SCROLL LOCK", "ЗАБОЙ" },
    { "BACKSPACE", "ТАБУЛЯЦИЯ" },
    { "LEFT ALT", "ЛЕВЫЙ АЛЬТ" },
    { "RIGHT ALT", "ПРАВЫЙ АЛЬТ" },
    { "LEFT SHIFT", "ЛЕВЫЙ ШИФТ" },
    { "RIGHT SHIFT", "ПРАВЫЙ ШИФТ" },
    { "SPACE BAR", "ЭСКЕЙП" },
    { "RETURN", "ВВОД" },
    { "LEFT CTRL", "ЛЕВЫЙ КТРЛ" },
    { "RIGHT CTRL", "ПРАВЫЙ КТРЛ" },
    { "CAPS LOCK", "ПРОБЕЛ" },
    { "NUMPAD *", "НУМПАД *" },
    { "NUMPAD +", "НУМПАД +" },
    { "COMMA ,", "VIRGULE ," },
    { "NUMPAD -", "НУМПАД -" },
    { "NUMPAD .", "НУМПАД ." },
    { "NUMPAD /", "НУМПАД /" },
    { "SEMICOLON ;", "ТОЧКА С ЗАПЯТОЙ" },
    { "EQUAL =", "РАВНО =" },
    { "MINUS -", "МИНУС" },
    { "LEFT BRACKET [", "ЛЕВАЯ СКОБКА [" },
    { "BACKSLASH \\", "ОБР. СЛЭШ \\" },
    { "RIGHT BRACKET ]", "ПРАВАЯ СКОБКА ]" },
    { "NUMPAD =", "НУМПАД =" },
    { "NUMPAD ENTER", "НУМПАД ВВОД" },
    { "SYS REQUEST", "СИСТ." },
    { "HOME", "ДОМОЙ" },
    { "UP ARROW", "ВВЕРХ" },
    { "PAGE UP", "СТР. ВВЕРХ" },
    { "LEFT ARROW", "ВЛЕВО" },
    { "RIGHT ARROW", "ВПРАВО" },
    { "END", "КОН" },
    { "DOWN ARROW", "ВНИЗ" },
    { "PAGE DOWN", "СТР. ВНИЗ" },
    { "INSERT", "ВСТАВ." },
    { "DELETE", "УДАЛ." },
    { "LEFT WINDOWS", "ЛЕВЫЕ ВИНДЫ" },
    { "RIGHT WINDOWS", "ПРАВЫЕ ВИНДЫ" },
    { "APPS MENU", "MENU APPS" },
    { "MOUSE X AXIS", "X ОСЬ МЫШИ" },
    { "MOUSE Y AXIS", "Y ОСЬ МЫШИ" },
    { "MOUSE Z AXIS", "Z ОСЬ МЫШИ" },
    { "MOUSE LEFT BUTTON", "ЛЕВАЯ КНОПКА МЫШИ" },
    { "MOUSE RIGHT BUTTON", "ПРАВАЯ КНОПКА МЫШИ" },
    { "MOUSE CENTER BUTTON", "ЦЕНТР. КНОПКА МЫШИ" },
    { "MOUSE EXTRA BUTTON", "ЭКСТРА КНОПКА МЫШИ" },
};

// Windows-1251 has the Cyrillic A to ya (two UTF-8 bytes each) in order from 0xc0; the texts use nothing else outside ASCII.
std::string ToWindows1251(const std::string_view Utf8)
{
    constexpr unsigned char AsciiEnd = 0x80;
    constexpr unsigned char LeadPayload = 0x1f;
    constexpr unsigned char ContinuationPayload = 0x3f;
    constexpr int ContinuationBits = 6;
    constexpr char32_t CyrillicA = U'А';
    constexpr char32_t CyrillicYa = U'я';
    constexpr unsigned char Windows1251A = 0xc0;
    std::string Result;
    for (size_t Index = 0; Index < Utf8.size(); ++Index)
    {
        const unsigned char Lead = static_cast<unsigned char>(Utf8[Index]);
        if (Lead < AsciiEnd)
        {
            Result += char(Lead);
            continue;
        }

        const char32_t Code = Index + 1 < Utf8.size() ? char32_t(Lead & LeadPayload) << ContinuationBits | char32_t(Utf8[Index + 1] & ContinuationPayload) : 0;
        Result += Code >= CyrillicA && Code <= CyrillicYa ? char(Windows1251A + (Code - CyrillicA)) : '?';
        ++Index;
    }

    return Result;
}

// The port's own texts, which the exe never had: co-op's. A text missing here shows in English.
constexpr FTranslation PortTranslations[] = {
    { "%s is back !", "%s снова в строю !" },
    { "%s is down !\nGet close to bring them back.", "%s подбит !\nПодлетите ближе, чтобы помочь." },
    { "Back in the fight !", "Снова в бою !" },
    { "You are down !\nStay close to a teammate to get back up.", "Вы подбиты !\nДержитесь рядом с напарником, чтобы подняться." },
};

const std::unordered_map<std::string_view, std::string>& RussianTexts()
{
    static const std::unordered_map<std::string_view, std::string> Texts = []
    {
        std::unordered_map<std::string_view, std::string> Result;
        for (const FTranslation& Translation : Translations)
        {
            Result.emplace(Translation.English, ToWindows1251(Translation.Russian));
        }

        for (const FTranslation& Translation : PortTranslations)
        {
            Result.emplace(Translation.English, ToWindows1251(Translation.Russian));
        }

        return Result;
    }();
    return Texts;
}
}

void SetLanguage(const ELanguage Language)
{
    CurrentLanguage = Language;
}

ELanguage GetLanguage()
{
    return CurrentLanguage;
}

std::string_view Localize(const std::string_view English)
{
    if (CurrentLanguage != ELanguage::Russian)
    {
        return English;
    }

    const auto& Texts = RussianTexts();
    const auto Found = Texts.find(English);
    return Found != Texts.end() ? std::string_view(Found->second) : English;
}

std::string_view TextSuffix()
{
    return CurrentLanguage == ELanguage::Russian ? "fr" : "en";
}

std::string FormatText(const std::string_view English, const std::span<const FFormatArgument> Arguments)
{
    const std::string_view Pattern = Localize(English);
    std::string Result;
    size_t Next = 0;
    for (size_t Index = 0; Index < Pattern.size(); ++Index)
    {
        if (Pattern[Index] != '%' || Index + 1 == Pattern.size())
        {
            Result += Pattern[Index];
            continue;
        }

        if (Pattern[Index + 1] == '%')
        {
            Result += '%';
            ++Index;
            continue;
        }

        // %[0][width][.precision]conversion
        size_t End = Index + 1;
        const bool bZeroFill = Pattern[End] == '0';
        End += bZeroFill ? 1 : 0;
        int Width = 0;
        End = size_t(std::from_chars(Pattern.data() + End, Pattern.data() + Pattern.size(), Width).ptr - Pattern.data());
        int Precision = -1;
        if (End < Pattern.size() && Pattern[End] == '.')
        {
            Precision = 0;
            End = size_t(std::from_chars(Pattern.data() + End + 1, Pattern.data() + Pattern.size(), Precision).ptr - Pattern.data());
        }

        if (End >= Pattern.size())
        {
            Result += Pattern.substr(Index);
            break;
        }

        const char Conversion = Pattern[End];
        const FFormatArgument Argument = Next < Arguments.size() ? Arguments[Next] : FFormatArgument(std::string_view());
        ++Next;
        std::string Piece;
        if (Conversion == 's')
        {
            Piece = std::holds_alternative<std::string_view>(Argument) ? std::string(std::get<std::string_view>(Argument)) : std::string();
        }
        else if (Conversion == 'f')
        {
            const double Value = std::holds_alternative<double>(Argument) ? std::get<double>(Argument) : 0.;
            // printf's precision for %f without one.
            constexpr int DefaultPrecision = 6;
            Piece = std::format("{:.{}f}", Value, Precision < 0 ? DefaultPrecision : Precision);
        }
        else if (std::holds_alternative<int64_t>(Argument))
        {
            Piece = std::to_string(std::get<int64_t>(Argument));
        }

        const char Fill = bZeroFill && Conversion != 's' ? '0' : ' ';
        if (int(Piece.size()) < Width)
        {
            const size_t Pad = size_t(Width) - Piece.size();
            Piece.insert(Fill == '0' && Piece.starts_with('-') ? 1 : 0, Pad, Fill);
        }

        Result += Piece;
        Index = End;
    }

    return Result;
}
