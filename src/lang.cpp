// Translations of the mod's own text. Columns: English, German, French, Italian, Spanish,
// Portuguese (Brazil), Polish, Russian, Ukrainian, Turkish, Japanese, Korean, Simplified Chinese,
// Traditional Chinese, Thai. Only plain ASCII punctuation and quotes, which every game font has.
// Built with /utf-8: the literals are UTF-8, as the game's UI text is.

#include "lang.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace umm_lang {
namespace {

constexpr int kLangs = static_cast<int>(Lang::Count);
using Row = const char* [kLangs];

// clang-format off
const Row kTable[] = {
    // MapKeyTitle
    {"MAP KEY & FILTERS", "KARTENLEGENDE & FILTER", "LEGENDE & FILTRES", "LEGENDA & FILTRI", "LEYENDA Y FILTROS",
     "LEGENDA E FILTROS", "LEGENDA I FILTRY", "ЛЕГЕНДА И ФИЛЬТРЫ", "ЛЕГЕНДА Й ФІЛЬТРИ", "LEJANT & FİLTRELER",
     "凡例とフィルター", "범례 및 필터", "图例与筛选", "圖例與篩選", "คำอธิบายและตัวกรอง"},
    // Find
    {"FIND", "SUCHEN", "CHERCHER", "CERCA", "BUSCAR", "BUSCAR", "SZUKAJ", "ПОИСК", "ПОШУК", "ARA",
     "検索", "검색", "搜索", "搜尋", "ค้นหา"},
    // FindTitle
    {"Find", "Suchen", "Chercher", "Cerca", "Buscar", "Buscar", "Szukaj", "Поиск", "Пошук", "Ara",
     "検索", "검색", "搜索", "搜尋", "ค้นหา"},
    // Cancel
    {"Cancel", "Abbrechen", "Annuler", "Annulla", "Cancelar", "Cancelar", "Anuluj", "Отмена", "Скасувати", "İptal",
     "キャンセル", "취소", "取消", "取消", "ยกเลิก"},
    // ShowAll
    {"SHOW ALL", "ALLE ZEIGEN", "TOUT AFFICHER", "MOSTRA TUTTO", "MOSTRAR TODO", "MOSTRAR TUDO", "POKAŻ WSZYSTKO",
     "ПОКАЗАТЬ ВСЁ", "ПОКАЗАТИ ВСЕ", "HEPSİNİ GÖSTER", "すべて表示", "모두 표시", "全部显示", "全部顯示", "แสดงทั้งหมด"},
    // HideAll
    {"HIDE ALL", "ALLE AUSBLENDEN", "TOUT MASQUER", "NASCONDI TUTTO", "OCULTAR TODO", "OCULTAR TUDO", "UKRYJ WSZYSTKO",
     "СКРЫТЬ ВСЁ", "СХОВАТИ ВСЕ", "HEPSİNİ GİZLE", "すべて非表示", "모두 숨기기", "全部隐藏", "全部隱藏", "ซ่อนทั้งหมด"},
    // Fog
    {"FOG:", "NEBEL:", "BRUME :", "NEBBIA:", "NIEBLA:", "NÉVOA:", "MGŁA:", "ТУМАН:", "ТУМАН:", "SİS:",
     "霧:", "안개:", "迷雾:", "迷霧:", "หมอก:"},
    // FogExplored
    {"EXPLORED", "ERKUNDET", "EXPLORÉ", "ESPLORATO", "EXPLORADO", "EXPLORADO", "ODKRYTE", "ИЗУЧЕНО", "ДОСЛІДЖЕНО",
     "KEŞFEDİLEN", "探索済み", "탐험함", "已探索", "已探索", "สำรวจแล้ว"},
    // FogHybrid
    {"HYBRID", "HYBRID", "HYBRIDE", "IBRIDO", "HÍBRIDO", "HÍBRIDO", "HYBRYDA", "ГИБРИД", "ГІБРИД", "KARMA",
     "ハイブリッド", "혼합", "混合", "混合", "ผสม"},
    // FogRevealAll
    {"REVEAL ALL", "ALLES AUFDECKEN", "TOUT RÉVÉLER", "RIVELA TUTTO", "REVELAR TODO", "REVELAR TUDO", "ODKRYJ WSZYSTKO",
     "ВСЯ КАРТА", "ВСЯ МАПА", "TÜMÜNÜ AÇ", "全て表示", "전체 공개", "全部揭示", "全部揭示", "เปิดทั้งหมด"},
    // On
    {"ON", "AN", "OUI", "SÌ", "SÍ", "SIM", "WŁ.", "ВКЛ", "ТАК", "AÇIK", "オン", "켜짐", "开", "開", "เปิด"},
    // Off
    {"OFF", "AUS", "NON", "NO", "NO", "NÃO", "WYŁ.", "ВЫКЛ", "НІ", "KAPALI", "オフ", "꺼짐", "关", "關", "ปิด"},
    // CatFastTravel
    {"FAST TRAVEL & ALTARS", "SCHNELLREISE & ALTÄRE", "VOYAGE RAPIDE & AUTELS", "VIAGGIO RAPIDO E ALTARI",
     "VIAJE RÁPIDO Y ALTARES", "VIAGEM RÁPIDA E ALTARES", "SZYBKA PODRÓŻ I OŁTARZE", "ПЕРЕМЕЩЕНИЕ И АЛТАРИ",
     "ШВИДКА ПОДОРОЖ І ВІВТАРІ", "HIZLI SEYAHAT & SUNAKLAR", "ファストトラベルと祭壇", "빠른 이동 및 제단",
     "快速旅行与祭坛", "快速旅行與祭壇", "เดินทางด่วนและแท่นบูชา"},
    // CatShroud
    {"SHROUD & VAULTS", "NEBEL & GEWÖLBE", "BRUME & CAVEAUX", "COLTRE E CRIPTE", "BRUMA Y CÁMARAS", "BRUMA E CÂMARAS",
     "MGŁA I KRYPTY", "ПЕЛЕНА И ХРАНИЛИЩА", "ПЕЛЕНА Й СХОВИЩА", "SİS & MAHZENLER", "シュラウドと宝物庫",
     "장막 및 금고", "雾霭与宝库", "霧靄與寶庫", "ม่านหมอกและห้องนิรภัย"},
    // CatWorld
    {"WORLD & RESOURCES", "WELT & RESSOURCEN", "MONDE & RESSOURCES", "MONDO E RISORSE", "MUNDO Y RECURSOS",
     "MUNDO E RECURSOS", "ŚWIAT I ZASOBY", "МИР И РЕСУРСЫ", "СВІТ І РЕСУРСИ", "DÜNYA & KAYNAKLAR",
     "ワールドと資源", "월드 및 자원", "世界与资源", "世界與資源", "โลกและทรัพยากร"},
    // CatQuests
    {"QUESTS & RUMORS", "QUESTS & GERÜCHTE", "QUÊTES & RUMEURS", "MISSIONI E VOCI", "MISIONES Y RUMORES",
     "MISSÕES E RUMORES", "ZADANIA I PLOTKI", "ЗАДАНИЯ И СЛУХИ", "ЗАВДАННЯ Й ЧУТКИ", "GÖREVLER & SÖYLENTİLER",
     "クエストと噂", "퀘스트 및 소문", "任务与传闻", "任務與傳聞", "เควสต์และข่าวลือ"},
    // CatCustomPins
    {"CUSTOM PINS", "EIGENE MARKIERUNGEN", "REPÈRES PERSO", "SEGNALINI PERSONALI", "MARCADORES PROPIOS",
     "MARCADORES PRÓPRIOS", "WŁASNE ZNACZNIKI", "СВОИ МЕТКИ", "ВЛАСНІ МІТКИ", "ÖZEL İŞARETLER", "カスタムピン",
     "사용자 핀", "自定义标记", "自訂標記", "หมุดที่กำหนดเอง"},
    // AncientSpire
    {"Ancient Spire", "Uralter Turm", "Flèche ancienne", "Guglia antica", "Aguja antigua", "Torre ancestral",
     "Starożytna iglica", "Древний шпиль", "Давній шпиль", "Kadim Kule", "古代の尖塔", "고대 첨탑", "古代尖塔",
     "古代尖塔", "ยอดหอโบราณ"},
    // FlameAltarHome
    {"Flame Altar (Home)", "Flammenaltar (Zuhause)", "Autel de la Flamme (maison)", "Altare della Fiamma (casa)",
     "Altar de la Llama (hogar)", "Altar da Chama (lar)", "Ołtarz Płomienia (dom)", "Алтарь пламени (дом)",
     "Вівтар полум'я (дім)", "Alev Sunağı (Ev)", "炎の祭壇 (拠点)", "불꽃 제단 (집)", "火焰祭坛 (家)", "火焰祭壇 (家)",
     "แท่นบูชาเปลวไฟ (บ้าน)"},
    // FlameSanctum
    {"Flame Sanctum", "Flammenheiligtum", "Sanctuaire de la Flamme", "Santuario della Fiamma", "Santuario de la Llama",
     "Santuário da Chama", "Sanktuarium Płomienia", "Святилище пламени", "Святилище полум'я", "Alev Tapınağı",
     "炎の聖域", "불꽃 성소", "火焰圣所", "火焰聖所", "วิหารเปลวไฟ"},
    // NightSanctum
    {"Night Sanctum", "Nachtheiligtum", "Sanctuaire nocturne", "Santuario notturno", "Santuario nocturno",
     "Santuário noturno", "Sanktuarium Nocy", "Ночное святилище", "Нічне святилище", "Gece Tapınağı", "夜の聖域",
     "밤의 성소", "夜之圣所", "夜之聖所", "วิหารรัตติกาล"},
    // GemForge
    {"Gem Forge", "Edelsteinschmiede", "Forge à gemmes", "Forgia delle gemme", "Forja de gemas", "Forja de gemas",
     "Kuźnia klejnotów", "Кузня самоцветов", "Кузня самоцвітів", "Mücevher Ocağı", "宝石の炉", "보석 대장간",
     "宝石熔炉", "寶石熔爐", "เตาหลอมอัญมณี"},
    // AncientObelisk
    {"Ancient Obelisk", "Uralter Obelisk", "Obélisque ancien", "Obelisco antico", "Obelisco antiguo",
     "Obelisco ancestral", "Starożytny obelisk", "Древний обелиск", "Давній обеліск", "Kadim Dikilitaş",
     "古代のオベリスク", "고대 오벨리스크", "古代方尖碑", "古代方尖碑", "เสาโอเบลิสก์โบราณ"},
    // WaterSource
    {"Water Source", "Wasserquelle", "Source d'eau", "Fonte d'acqua", "Fuente de agua", "Fonte de água",
     "Źródło wody", "Источник воды", "Джерело води", "Su Kaynağı", "水源", "수원", "水源", "水源", "แหล่งน้ำ"},
    // FastTravelPoint
    {"Fast Travel Point", "Schnellreisepunkt", "Point de voyage rapide", "Punto di viaggio rapido",
     "Punto de viaje rápido", "Ponto de viagem rápida", "Punkt szybkiej podróży", "Точка перемещения",
     "Точка швидкої подорожі", "Hızlı Seyahat Noktası", "ファストトラベル地点", "빠른 이동 지점", "快速旅行点",
     "快速旅行點", "จุดเดินทางด่วน"},
    // ElixirWell
    {"Elixir Well / Root", "Elixierquelle / Wurzel", "Puits d'élixir / Racine", "Pozzo d'elisir / Radice",
     "Pozo de elixir / Raíz", "Poço de elixir / Raiz", "Studnia eliksiru / Korzeń", "Колодец эликсира / Корень",
     "Криниця еліксиру / Корінь", "Eliksir Kuyusu / Kök", "エリクサーの泉 / 根", "엘릭서 우물 / 뿌리",
     "灵药之井 / 根", "靈藥之井 / 根", "บ่อยาอายุวัฒนะ / ราก"},
    // CatacombsTomb
    {"Catacombs / Tomb", "Katakomben / Grab", "Catacombes / Tombeau", "Catacombe / Tomba", "Catacumbas / Tumba",
     "Catacumbas / Tumba", "Katakumby / Grobowiec", "Катакомбы / Гробница", "Катакомби / Гробниця",
     "Yeraltı Mezarı / Türbe", "地下墓地 / 墓", "카타콤 / 무덤", "地下墓穴 / 陵墓", "地下墓穴 / 陵墓",
     "สุสานใต้ดิน / หลุมฝังศพ"},
    // AncientVault
    {"Ancient Vault", "Uraltes Gewölbe", "Caveau ancien", "Cripta antica", "Cámara antigua", "Câmara ancestral",
     "Starożytny skarbiec", "Древнее хранилище", "Давнє сховище", "Kadim Mahzen", "古代の宝物庫", "고대 금고",
     "古代宝库", "古代寶庫", "ห้องนิรภัยโบราณ"},
    // ShroudLair
    {"Shroud Lair", "Nebelhort", "Antre de la brume", "Covo della coltre", "Guarida de la bruma", "Covil da bruma",
     "Leże mgły", "Логово пелены", "Лігво пелени", "Sis İni", "シュラウドの巣", "장막 소굴", "雾霭巢穴", "霧靄巢穴",
     "รังม่านหมอก"},
    // HollowHalls
    {"Hollow Halls", "Hohle Hallen", "Salles creuses", "Sale cave", "Salas huecas", "Salões ocos", "Puste komnaty",
     "Полые залы", "Порожні зали", "Oyuk Salonlar", "虚ろな広間", "텅 빈 전당", "空洞大厅", "空洞大廳", "ห้องโถงกลวง"},
    // HiddenTomb
    {"Hidden Tomb", "Verborgenes Grab", "Tombeau caché", "Tomba nascosta", "Tumba oculta", "Tumba oculta",
     "Ukryty grobowiec", "Скрытая гробница", "Прихована гробниця", "Gizli Türbe", "隠された墓", "숨겨진 무덤",
     "隐秘陵墓", "隱密陵墓", "หลุมฝังศพลับ"},
    // GoldChest
    {"Gold Chest", "Goldtruhe", "Coffre en or", "Forziere d'oro", "Cofre de oro", "Baú de ouro", "Złota skrzynia",
     "Золотой сундук", "Золота скриня", "Altın Sandık", "金の宝箱", "금 상자", "金宝箱", "金寶箱", "หีบทอง"},
    // SilverChest
    {"Silver Chest", "Silbertruhe", "Coffre en argent", "Forziere d'argento", "Cofre de plata", "Baú de prata",
     "Srebrna skrzynia", "Серебряный сундук", "Срібна скриня", "Gümüş Sandık", "銀の宝箱", "은 상자", "银宝箱",
     "銀寶箱", "หีบเงิน"},
    // TownSettlement
    {"Town / Settlement", "Stadt / Siedlung", "Ville / Colonie", "Città / Insediamento", "Pueblo / Asentamiento",
     "Cidade / Povoado", "Miasto / Osada", "Город / Поселение", "Місто / Поселення", "Kasaba / Yerleşim",
     "町 / 集落", "마을 / 정착지", "城镇 / 聚落", "城鎮 / 聚落", "เมือง / ชุมชน"},
    // Farmstead
    {"Farmstead", "Gehöft", "Ferme", "Fattoria", "Granja", "Fazenda", "Gospodarstwo", "Ферма", "Хутір", "Çiftlik",
     "農場", "농장", "农庄", "農莊", "ฟาร์ม"},
    // SurvivorCamp
    {"Survivor Camp", "Überlebendenlager", "Camp de survivants", "Campo dei sopravvissuti",
     "Campamento de supervivientes", "Acampamento de sobreviventes", "Obóz ocalałych", "Лагерь выживших",
     "Табір уцілілих", "Hayatta Kalan Kampı", "生存者のキャンプ", "생존자 야영지", "幸存者营地", "倖存者營地",
     "ค่ายผู้รอดชีวิต"},
    // MineQuarry
    {"Mine / Quarry", "Mine / Steinbruch", "Mine / Carrière", "Miniera / Cava", "Mina / Cantera", "Mina / Pedreira",
     "Kopalnia / Kamieniołom", "Шахта / Карьер", "Шахта / Кар'єр", "Maden / Taş Ocağı", "鉱山 / 採石場",
     "광산 / 채석장", "矿井 / 采石场", "礦坑 / 採石場", "เหมือง / เหมืองหิน"},
    // CavePassage
    {"Cave Passage", "Höhlendurchgang", "Passage souterrain", "Passaggio in grotta", "Paso de cueva",
     "Passagem de caverna", "Przejście jaskiniowe", "Пещерный проход", "Печерний прохід", "Mağara Geçidi",
     "洞窟の通路", "동굴 통로", "洞穴通道", "洞穴通道", "ทางผ่านถ้ำ"},
    // EnemyEncampment
    {"Enemy Encampment", "Feindliches Lager", "Campement ennemi", "Accampamento nemico", "Campamento enemigo",
     "Acampamento inimigo", "Obóz wroga", "Вражеский лагерь", "Ворожий табір", "Düşman Karargâhı", "敵の野営地",
     "적 야영지", "敌方营地", "敵方營地", "ค่ายศัตรู"},
    // BalefireGarrison
    {"Balefire Garrison", "Unheilsfeuer-Garnison", "Garnison du feu maudit", "Guarnigione del fuoco funesto",
     "Guarnición de fuego funesto", "Guarnição do fogo funesto", "Garnizon złowrogiego ognia",
     "Гарнизон зловещего огня", "Гарнізон зловісного вогню", "Uğursuz Ateş Garnizonu", "凶火の駐屯地",
     "흉화 주둔지", "凶火驻地", "凶火駐地", "ป้อมไฟอัปมงคล"},
    // MainQuest
    {"Main Quest", "Hauptquest", "Quête principale", "Missione principale", "Misión principal", "Missão principal",
     "Zadanie główne", "Основное задание", "Основне завдання", "Ana Görev", "メインクエスト", "메인 퀘스트",
     "主线任务", "主線任務", "เควสต์หลัก"},
    // SideQuest
    {"Side Quest", "Nebenquest", "Quête secondaire", "Missione secondaria", "Misión secundaria", "Missão secundária",
     "Zadanie poboczne", "Побочное задание", "Побічне завдання", "Yan Görev", "サイドクエスト", "사이드 퀘스트",
     "支线任务", "支線任務", "เควสต์รอง"},
    // LoreRumor
    {"Lore Rumor (?)", "Gerücht (?)", "Rumeur (?)", "Voce (?)", "Rumor (?)", "Rumor (?)", "Plotka (?)", "Слух (?)",
     "Чутка (?)", "Söylenti (?)", "噂 (?)", "소문 (?)", "传闻 (?)", "傳聞 (?)", "ข่าวลือ (?)"},
    // PlayerGrave
    {"Player Grave", "Spielergrab", "Tombe de joueur", "Tomba del giocatore", "Tumba de jugador", "Túmulo de jogador",
     "Grób gracza", "Могила игрока", "Могила гравця", "Oyuncu Mezarı", "プレイヤーの墓", "플레이어 무덤", "玩家墓碑",
     "玩家墓碑", "หลุมศพผู้เล่น"},
    // OtherPlayers
    {"Other Players", "Andere Spieler", "Autres joueurs", "Altri giocatori", "Otros jugadores", "Outros jogadores",
     "Inni gracze", "Другие игроки", "Інші гравці", "Diğer Oyuncular", "他のプレイヤー", "다른 플레이어", "其他玩家",
     "其他玩家", "ผู้เล่นอื่น"},
    // GoldSarcophagus
    {"Gold Sarcophagus", "Goldsarkophag", "Sarcophage en or", "Sarcofago d'oro", "Sarcófago de oro",
     "Sarcófago de ouro", "Złoty sarkofag", "Золотой саркофаг", "Золотий саркофаг", "Altın Lahit", "金の石棺",
     "금 석관", "金石棺", "金石棺", "โลงศพทอง"},
    // SilverSarcophagus
    {"Silver Sarcophagus", "Silbersarkophag", "Sarcophage en argent", "Sarcofago d'argento", "Sarcófago de plata",
     "Sarcófago de prata", "Srebrny sarkofag", "Серебряный саркофаг", "Срібний саркофаг", "Gümüş Lahit", "銀の石棺",
     "은 석관", "银石棺", "銀石棺", "โลงศพเงิน"},
    // ChestLocked (appended to a chest name)
    {" (Locked)", " (verschlossen)", " (verrouillé)", " (chiuso)", " (cerrado)", " (trancado)", " (zamknięta)",
     " (заперт)", " (замкнено)", " (kilitli)", " (施錠)", " (잠김)", " (已上锁)", " (已上鎖)", " (ล็อก)"},
    // CustomPin
    {"Custom Pin", "Eigene Markierung", "Repère perso", "Segnalino personale", "Marcador propio", "Marcador próprio",
     "Własny znacznik", "Своя метка", "Власна мітка", "Özel İşaret", "カスタムピン", "사용자 핀", "自定义标记",
     "自訂標記", "หมุดที่กำหนดเอง"},
    // MapLocation
    {"Map Location", "Kartenort", "Lieu", "Luogo", "Lugar", "Local", "Miejsce", "Место", "Місце", "Konum", "地点",
     "장소", "地点", "地點", "สถานที่"},
    // OtherPlayer
    {"Other Player", "Anderer Spieler", "Autre joueur", "Altro giocatore", "Otro jugador", "Outro jogador",
     "Inny gracz", "Другой игрок", "Інший гравець", "Diğer Oyuncu", "他のプレイヤー", "다른 플레이어", "其他玩家",
     "其他玩家", "ผู้เล่นอื่น"},
    // TypeToSearch
    {"Type to search", "Tippen zum Suchen", "Tapez pour chercher", "Scrivi per cercare", "Escribe para buscar",
     "Digite para buscar", "Wpisz, aby szukać", "Введите для поиска", "Введіть для пошуку", "Aramak için yazın",
     "入力して検索", "입력하여 검색", "输入以搜索", "輸入以搜尋", "พิมพ์เพื่อค้นหา"},
    // MatchesFmt (%zu; English uses its own plural instead)
    {"%zu matches", "Treffer: %zu", "Résultats : %zu", "Risultati: %zu", "Resultados: %zu", "Resultados: %zu",
     "Wyniki: %zu", "Найдено: %zu", "Знайдено: %zu", "Sonuç: %zu", "一致: %zu", "결과: %zu", "匹配: %zu", "符合: %zu",
     "พบ: %zu"},
    // HiddenFmt (%d)
    {" (%d hidden)", " (%d ausgeblendet)", " (%d masqués)", " (%d nascosti)", " (%d ocultos)", " (%d ocultos)",
     " (ukryte: %d)", " (скрыто: %d)", " (приховано: %d)", " (%d gizli)", " (非表示 %d)", " (숨김 %d)", " (隐藏 %d)",
     " (隱藏 %d)", " (ซ่อน %d)"},
    // FiltersFmt (%s = On / Off)
    {"F2: Filters [%s]", "F2: Filter [%s]", "F2 : Filtres [%s]", "F2: Filtri [%s]", "F2: Filtros [%s]",
     "F2: Filtros [%s]", "F2: Filtry [%s]", "F2: Фильтры [%s]", "F2: Фільтри [%s]", "F2: Filtreler [%s]",
     "F2: フィルター [%s]", "F2: 필터 [%s]", "F2: 筛选 [%s]", "F2: 篩選 [%s]", "F2: ตัวกรอง [%s]"},
    // AllMatchingFmt (%zu)
    {"All Matching Places (%zu) - Zoom to fit", "Alle Treffer (%zu) - Einpassen", "Tous les lieux (%zu) - Ajuster",
     "Tutti i luoghi (%zu) - Adatta", "Todos los lugares (%zu) - Ajustar", "Todos os locais (%zu) - Ajustar",
     "Wszystkie miejsca (%zu) - Dopasuj", "Все места (%zu) - Показать все", "Усі місця (%zu) - Показати всі",
     "Tüm yerler (%zu) - Sığdır", "一致する全地点 (%zu) - 全体表示", "모든 장소 (%zu) - 전체 보기",
     "所有匹配地点 (%zu) - 全部显示", "所有符合地點 (%zu) - 全部顯示", "สถานที่ทั้งหมด (%zu) - ซูมให้พอดี"},
    // AllNamedFmt (%s name, %zu count)
    {"All \"%s\" (%zu) - Zoom to fit", "Alle \"%s\" (%zu) - Einpassen", "Tous \"%s\" (%zu) - Ajuster",
     "Tutti \"%s\" (%zu) - Adatta", "Todos \"%s\" (%zu) - Ajustar", "Todos \"%s\" (%zu) - Ajustar",
     "Wszystkie \"%s\" (%zu) - Dopasuj", "Все \"%s\" (%zu) - Показать все", "Усі \"%s\" (%zu) - Показати всі",
     "Tüm \"%s\" (%zu) - Sığdır", "すべての \"%s\" (%zu) - 全体表示", "모든 \"%s\" (%zu) - 전체 보기",
     "所有 \"%s\" (%zu) - 全部显示", "所有 \"%s\" (%zu) - 全部顯示", "\"%s\" ทั้งหมด (%zu) - ซูมให้พอดี"},
    // ZeroVisibleFmt (%d)
    {"0 visible (%d hidden) - F2/Enter to show", "0 sichtbar (%d ausgeblendet) - F2/Enter zum Anzeigen",
     "0 visible (%d masqués) - F2/Entrée pour afficher", "0 visibili (%d nascosti) - F2/Invio per mostrare",
     "0 visibles (%d ocultos) - F2/Intro para mostrar", "0 visíveis (%d ocultos) - F2/Enter para mostrar",
     "0 widocznych (ukryte: %d) - F2/Enter, aby pokazać", "0 видно (скрыто: %d) - F2/Enter, чтобы показать",
     "0 видно (приховано: %d) - F2/Enter, щоб показати", "0 görünür (%d gizli) - göstermek için F2/Enter",
     "表示 0 (非表示 %d) - F2/Enterで表示", "표시 0 (숨김 %d) - F2/Enter로 표시", "可见 0 (隐藏 %d) - 按 F2/Enter 显示",
     "可見 0 (隱藏 %d) - 按 F2/Enter 顯示", "แสดง 0 (ซ่อน %d) - กด F2/Enter เพื่อแสดง"},
    // LockedTag
    {" [Locked]", " [Gesperrt]", " [Verrouillé]", " [Bloccato]", " [Bloqueado]", " [Bloqueado]", " [Zablokowane]",
     " [Закрыто]", " [Закрито]", " [Kilitli]", " [未解放]", " [잠김]", " [未解锁]", " [未解鎖]", " [ล็อก]"},
    // HiddenTag
    {" [Hidden]", " [Ausgeblendet]", " [Masqué]", " [Nascosto]", " [Oculto]", " [Oculto]", " [Ukryte]", " [Скрыто]",
     " [Приховано]", " [Gizli]", " [非表示]", " [숨김]", " [已隐藏]", " [已隱藏]", " [ซ่อน]"},
    // Day
    {"Day", "Tag", "Jour", "Giorno", "Día", "Dia", "Dzień", "День", "День", "Gündüz", "昼", "낮", "白天", "白天",
     "กลางวัน"},
    // Dawn
    {"Dawn", "Morgen", "Aube", "Alba", "Amanecer", "Amanhecer", "Świt", "Рассвет", "Світанок", "Şafak",
     "夜明け", "새벽", "黎明", "黎明", "รุ่งอรุณ"},
    // Dusk
    {"Dusk", "Abend", "Crépuscule", "Tramonto", "Atardecer", "Entardecer", "Zmierzch", "Сумерки", "Сутінки",
     "Akşam", "夕暮れ", "황혼", "黄昏", "黃昏", "พลบค่ำ"},
    // Night
    {"Night", "Nacht", "Nuit", "Notte", "Noche", "Noite", "Noc", "Ночь", "Ніч", "Gece", "夜", "밤", "夜晚", "夜晚",
     "กลางคืน"},
    // ZoomFmt (%s = 1.5x)
    {"Zoom: %s", "Zoom: %s", "Zoom : %s", "Zoom: %s", "Zoom: %s", "Zoom: %s", "Zoom: %s", "Зум: %s",
     "Зум: %s", "Zoom: %s", "ズーム: %s", "확대: %s", "缩放: %s", "縮放: %s", "ซูม: %s"},
    // CompassN, CompassS, CompassW, CompassE
    {"N", "N", "N", "N", "N", "N", "N", "С", "Пн", "K", "北", "북", "北", "北", "N"},
    {"S", "S", "S", "S", "S", "S", "S", "Ю", "Пд", "G", "南", "남", "南", "南", "S"},
    {"W", "W", "O", "O", "O", "O", "W", "З", "Зх", "B", "西", "서", "西", "西", "W"},
    {"E", "O", "E", "E", "E", "L", "E", "В", "Сх", "D", "東", "동", "东", "東", "E"},
};
// clang-format on

static_assert(sizeof(kTable) / sizeof(kTable[0]) == static_cast<size_t>(Str::Count),
              "one row per Str, in order");

// Steam's language codes, by Lang.
const char* const kSteamCodes[kLangs] = {
    "english", "german", "french", "italian", "spanish", "brazilian", "polish", "russian", "ukrainian", "turkish",
    "japanese", "koreana", "schinese", "tchinese", "thai",
};

Lang g_lang = Lang::English;
bool g_decided = false;
bool g_followsGame = false;

bool LangFromCode(const char* code, Lang& out) {
    if (!code || !*code) return false;
    char lower[32]{};
    for (size_t i = 0; i + 1 < sizeof(lower) && code[i]; ++i) {
        const char c = code[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    for (int i = 0; i < kLangs; ++i) {
        if (std::strcmp(lower, kSteamCodes[i]) == 0) { out = static_cast<Lang>(i); return true; }
    }
    // Steam languages the game has no text for, mapped to the nearest one it does.
    if (std::strcmp(lower, "latam") == 0) { out = Lang::Spanish; return true; }
    if (std::strcmp(lower, "portuguese") == 0) { out = Lang::Brazilian; return true; }
    return false;
}

using SteamAppsFn = void* (*)();
using GameLanguageFn = const char* (*)(void* apps);

// The game's Steam language; null while Steam is not up yet.
const char* SteamGameLanguage() {
    HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
    if (!steam) return nullptr;
    auto apps = reinterpret_cast<SteamAppsFn>(GetProcAddress(steam, "SteamAPI_SteamApps_v009"));
    if (!apps) apps = reinterpret_cast<SteamAppsFn>(GetProcAddress(steam, "SteamAPI_SteamApps_v008"));
    auto language = reinterpret_cast<GameLanguageFn>(GetProcAddress(steam, "SteamAPI_ISteamApps_GetCurrentGameLanguage"));
    if (!apps || !language) return nullptr;
    void* a = apps();
    return a ? language(a) : nullptr;
}

uint32_t NextCodepoint(const unsigned char*& p) {
    uint32_t c = *p++;
    if (c < 0x80) return c;
    int extra = (c >= 0xf0) ? 3 : (c >= 0xe0) ? 2 : (c >= 0xc0) ? 1 : 0;
    c &= (extra == 3) ? 0x07 : (extra == 2) ? 0x0f : 0x1f;
    while (extra-- > 0 && (*p & 0xc0) == 0x80) c = (c << 6) | (*p++ & 0x3f);
    return c;
}

bool IsWide(uint32_t c) {
    return (c >= 0x1100 && c <= 0x115f) || (c >= 0x2e80 && c <= 0xa4cf) || (c >= 0xac00 && c <= 0xd7a3) ||
           (c >= 0xf900 && c <= 0xfaff) || (c >= 0xff00 && c <= 0xff60);
}

bool IsThaiMark(uint32_t c) {
    return c == 0x0e31 || (c >= 0x0e34 && c <= 0x0e3a) || (c >= 0x0e47 && c <= 0x0e4e);
}

} // namespace

void EnsureLanguage(const char* configPath) {
    if (g_decided) return;
    if (configPath && *configPath) {
        char setting[32]{};
        GetPrivateProfileStringA("General", "Language", "auto", setting, sizeof(setting), configPath);
        Lang forced{};
        if (LangFromCode(setting, forced)) {
            g_lang = forced;
            g_decided = true;
            return;
        }
    }
    const char* steam = SteamGameLanguage();
    if (!steam) return; // ask again next frame
    Lang fromSteam{};
    g_lang = LangFromCode(steam, fromSteam) ? fromSteam : Lang::English;
    g_followsGame = true;
    g_decided = true;
}

bool Decided() { return g_decided; }

bool FollowsGame() { return g_followsGame; }

Lang Current() { return g_lang; }

const char* SteamCode(Lang lang) {
    const int i = static_cast<int>(lang);
    return (i >= 0 && i < kLangs) ? kSteamCodes[i] : "english";
}

const char* T(Str s) {
    const Row& row = kTable[static_cast<int>(s)];
    const char* text = row[static_cast<int>(g_lang)];
    return (text && *text) ? text : row[0];
}

std::string FormatMatches(size_t count) {
    char buf[96]{};
    if (g_lang == Lang::English) {
        std::snprintf(buf, sizeof(buf), "%zu match%s", count, count == 1 ? "" : "es");
    } else {
        std::snprintf(buf, sizeof(buf), T(Str::MatchesFmt), count);
    }
    return buf;
}

bool Uses12HourClock() { return g_lang == Lang::English; }

bool TallScript() {
    return g_lang == Lang::Japanese || g_lang == Lang::Korean || g_lang == Lang::SChinese ||
           g_lang == Lang::TChinese || g_lang == Lang::Thai;
}

float TextWidth(const char* utf8) {
    if (!utf8) return 0.0f;
    float w = 0.0f;
    const auto* p = reinterpret_cast<const unsigned char*>(utf8);
    while (*p) {
        const uint32_t c = NextCodepoint(p);
        if (c == ' ') w += 5.0f;
        else if (c == 'I' || c == 'J' || c == 'i' || c == 'j' || c == 'l' || c == '.' || c == ',' || c == ':' ||
                 c == '\'' || c == '!' || c == '|' || c == 0x0130 || c == 0x0131) w += 5.5f;
        else if (c == 'f' || c == 't' || c == 'r' || c == '(' || c == ')' || c == '[' || c == ']' || c == '/' ||
                 c == '-') w += 6.5f;
        // Cyrillic is wider in the game's font than Latin (measured on the Russian panel)
        else if (c == 0x0416 || c == 0x0428 || c == 0x0429 || c == 0x042b || c == 0x042e) w += 20.0f;
        else if (c == 0x0436 || c == 0x0448 || c == 0x0449 || c == 0x044b || c == 0x044e) w += 14.5f;
        else if ((c >= 0x0400 && c <= 0x042f) || (c >= 0x0490 && c <= 0x04ff && (c % 2) == 0)) w += 17.0f;
        else if ((c >= 0x0430 && c <= 0x045f) || (c >= 0x0490 && c <= 0x04ff)) w += 11.5f;
        else if (c == 'M' || c == 'W' || c == 'm' || c == 'w') w += 16.0f;
        else if (c >= '0' && c <= '9') w += 10.0f;
        else if ((c >= 'A' && c <= 'Z') || (c >= 0x00c0 && c <= 0x00de) || (c >= 0x0100 && c <= 0x017f && (c % 2) == 0))
            w += 14.0f;
        else if (IsWide(c)) w += 24.0f; // measured on the Japanese panel
        else if (IsThaiMark(c)) w += 0.0f;
        else if (c >= 0x0e00 && c <= 0x0e7f) w += 12.5f; // measured on the Thai panel
        else w += 9.0f;
    }
    return w;
}

size_t Columns(const std::string& utf8) {
    size_t cols = 0;
    const auto* p = reinterpret_cast<const unsigned char*>(utf8.c_str());
    while (*p) {
        const uint32_t c = NextCodepoint(p);
        cols += IsWide(c) ? 2 : (IsThaiMark(c) ? 0 : 1);
    }
    return cols;
}

std::string TruncateColumns(const std::string& utf8, size_t maxColumns) {
    if (Columns(utf8) <= maxColumns) return utf8;
    const size_t budget = maxColumns > 2 ? maxColumns - 2 : 0;
    size_t cols = 0;
    const auto* start = reinterpret_cast<const unsigned char*>(utf8.c_str());
    const auto* p = start;
    const unsigned char* cut = start;
    while (*p) {
        const auto* before = p;
        const uint32_t c = NextCodepoint(p);
        const size_t w = IsWide(c) ? 2 : (IsThaiMark(c) ? 0 : 1);
        if (cols + w > budget) break;
        cols += w;
        cut = p;
        (void)before;
    }
    std::string out(utf8, 0, static_cast<size_t>(cut - start));
    while (!out.empty() && (out.back() == ' ' || out.back() == '-')) out.pop_back();
    return out + "..";
}

std::string ToLowerUtf8(const std::string& utf8) {
    if (utf8.empty()) return utf8;
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wlen <= 0) return utf8;
    std::wstring w(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), wlen);
    LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, w.data(), wlen, w.data(), wlen, nullptr, nullptr, 0);
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return utf8;
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, out.data(), len, nullptr, nullptr);
    return out;
}

} // namespace umm_lang
