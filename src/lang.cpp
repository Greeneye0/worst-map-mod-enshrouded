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
    // CompletedQuests
    {"Completed Quests", "Abgeschlossene Quests", "Quêtes terminées", "Missioni completate", "Misiones completadas",
     "Missões concluídas", "Ukończone zadania", "Завершённые задания", "Завершені завдання", "Tamamlanan Görevler",
     "完了したクエスト", "완료된 퀘스트", "已完成任务", "已完成任務", "เควสต์ที่เสร็จแล้ว"},
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
    // Terrain
    {"Terrain", "Gelände", "Terrain", "Terreno", "Terreno", "Terreno", "Teren", "Рельеф",
     "Рельєф", "Arazi", "地形", "지형", "地形", "地形", "ภูมิประเทศ"},
    // CompassN, CompassS, CompassW, CompassE
    {"N", "N", "N", "N", "N", "N", "N", "С", "Пн", "K", "北", "북", "北", "北", "N"},
    {"S", "S", "S", "S", "S", "S", "S", "Ю", "Пд", "G", "南", "남", "南", "南", "S"},
    {"W", "W", "O", "O", "O", "O", "W", "З", "Зх", "B", "西", "서", "西", "西", "W"},
    {"E", "O", "E", "E", "E", "L", "E", "В", "Сх", "D", "東", "동", "东", "東", "E"},
    // Pins
    {"PINS:", "PINS:", "REPÈRES :", "PIN:", "PINES:", "PINOS:", "ZNACZNIKI:", "МЕТКИ:", "МІТКИ:", "PİNLER:",
     "ピン:", "핀:", "标记:", "標記:", "หมุด:"},
    // SettingsTitle
    {"SETTINGS", "EINSTELLUNGEN", "PARAMÈTRES", "IMPOSTAZIONI", "AJUSTES", "CONFIGURAÇÕES", "USTAWIENIA",
     "НАСТРОЙКИ", "НАЛАШТУВАННЯ", "AYARLAR", "設定", "설정", "设置", "設定", "การตั้งค่า"},
    // PinSize
    {"Custom Marker Size", "Größe eigener Marker", "Taille des repères", "Dimensione segnalini", "Tamaño de marcadores",
     "Tamanho dos marcadores", "Rozmiar znaczników", "Размер меток", "Розмір міток", "İşaret Boyutu",
     "マーカーサイズ", "마커 크기", "自定义标记大小", "自訂標記大小", "ขนาดหมุด"},
    // ShowMinimap
    {"Show Minimap", "Minikarte zeigen", "Afficher la minicarte", "Mostra minimappa", "Mostrar minimapa",
     "Mostrar minimapa", "Pokaż minimapę", "Показать миникарту", "Показати мінімапу", "Mini Haritayı Göster",
     "ミニマップ表示", "미니맵 표시", "显示小地图", "顯示小地圖", "แสดงแผนที่ย่อ"},
    // MinimapTerrain
    {"Minimap Terrain", "Minikarte Gelände", "Terrain minicarte", "Terreno minimappa", "Terreno del minimapa",
     "Terreno do minimapa", "Teren minimapy", "Рельеф миникарты", "Рельєф мінімапи", "Mini Harita Arazisi",
     "ミニマップ地形", "미니맵 지형", "小地图地形", "小地圖地形", "ภูมิประเทศแผนที่ย่อ"},
    // ClockIcon
    {"Minimap Icon", "Minikarte Symbol", "Icône minicarte", "Icona minimappa", "Icono del minimapa",
     "Ícone do minimapa", "Ikona minimapy", "Значок миникарты", "Значок мінімапи", "Mini Harita Simgesi",
     "ミニマップアイコン", "미니맵 아이콘", "小地图图标", "小地圖圖示", "ไอคอนแผนที่ย่อ"},
    // SunMoon
    {"Sun / Moon", "Sonne / Mond", "Soleil / Lune", "Sole / Luna", "Sol / Luna", "Sol / Lua", "Słońce / Księżyc",
     "Солнце / Луна", "Сонце / Місяць", "Güneş / Ay", "太陽 / 月", "해 / 달", "日月", "日月", "อาทิตย์ / จันทร์"},
    // Weather
    {"Weather", "Wetter", "Météo", "Meteo", "Clima", "Clima", "Pogoda", "Погода", "Погода", "Hava Durumu",
     "天気", "날씨", "天气", "天氣", "สภาพอากาศ"},
    // LootedChests
    {"Looted Chests", "Geplünderte Truhen", "Coffres pillés", "Forzieri saccheggiati", "Cofres saqueados",
     "Baús saqueados", "Otwarte skrzynie", "Открытые сундуки", "Відкриті скрині", "Yağmalanan Sandıklar",
     "開封済みの宝箱", "획득한 상자", "已开启宝箱", "已開啟寶箱", "หีบที่เปิดแล้ว"},
    // Hide
    {"Hide", "Ausblenden", "Masquer", "Nascondi", "Ocultar", "Ocultar", "Ukryj", "Скрыть", "Сховати", "Gizle",
     "非表示", "숨김", "隐藏", "隱藏", "ซ่อน"},
    // GreyedOut
    {"Grayed Out", "Ausgegraut", "Grisé", "Oscurato", "Atenuado", "Acinzentado", "Wyszarzone", "Серый", "Сірий",
     "Soluk", "グレー表示", "회색 표시", "变灰", "變灰", "สีเทา"},
    // WeatherClear
    {"Clear", "Klar", "Dégagé", "Sereno", "Despejado", "Limpo", "Pogodnie", "Ясно", "Ясно", "Açık",
     "晴れ", "맑음", "晴朗", "晴朗", "แจ่มใส"},
    // WeatherRain
    {"Rain", "Regen", "Pluie", "Pioggia", "Lluvia", "Chuva", "Deszcz", "Дождь", "Дощ", "Yağmur",
     "雨", "비", "雨", "雨", "ฝนตก"},
    // WeatherStorm
    {"Storm", "Sturm", "Orage", "Tempesta", "Tormenta", "Tempestade", "Burza", "Гроза", "Гроза", "Fırtına",
     "嵐", "폭풍", "暴风雨", "暴風雨", "พายุ"},
    // WeatherSnow
    {"Snow", "Schnee", "Neige", "Neve", "Nieve", "Neve", "Śnieg", "Снег", "Сніг", "Kar",
     "雪", "눈", "雪", "雪", "หิมะ"},
    // WeatherFog
    {"Fog", "Nebel", "Brouillard", "Nebbia", "Niebla", "Neblina", "Mgła", "Туман", "Туман", "Sis",
     "霧", "안개", "雾", "霧", "หมอก"},
    // HintClearChest (the map's hint bar, with Ctrl + left click): mark or unmark a chest as looted
    {"Mark Chest Looted", "Truhe abhaken", "Marquer coffre pillé", "Segna forziere saccheggiato",
     "Marcar cofre saqueado", "Marcar baú saqueado", "Oznacz otwartą skrzynię", "Отметить сундук открытым",
     "Позначити скриню відкритою", "Sandığı yağmalandı işaretle", "宝箱を開封済みにする", "상자 획득 표시",
     "标记已开启宝箱", "標記已開啟寶箱", "ทำเครื่องหมายหีบที่เปิดแล้ว"},
    // HintToggleKey (Tab): show or hide the Map Key panel
    {"Show/Hide Map Key", "Legende ein/aus", "Afficher/masquer légende", "Mostra/nascondi legenda",
     "Mostrar/ocultar leyenda", "Mostrar/ocultar legenda", "Pokaż/ukryj legendę", "Показать/скрыть легенду",
     "Показати/сховати легенду", "Lejantı göster/gizle", "凡例の表示切替", "범례 표시/숨기기",
     "显示/隐藏图例", "顯示/隱藏圖例", "แสดง/ซ่อนคำอธิบาย"},
    // HintFind (Shift+F): the Find button's text, in the hint bar's sentence case
    {"Find", "Suchen", "Chercher", "Cerca", "Buscar", "Buscar", "Szukaj", "Поиск", "Пошук", "Ara",
     "検索", "검색", "搜索", "搜尋", "ค้นหา"},
    // CtrlClick
    {"Ctrl+Click", "Strg+Klick", "Ctrl+Clic", "Ctrl+Clic", "Ctrl+Clic", "Ctrl+Clique", "Ctrl+Klik",
     "Ctrl+Клик", "Ctrl+Клік", "Ctrl+Tık", "Ctrl+クリック", "Ctrl+클릭", "Ctrl+点击", "Ctrl+點擊", "Ctrl+คลิก"},
    // CatMining
    {"MINING", "BERGBAU", "MINERAIS", "MINIERA", "MINERÍA", "MINERAÇÃO", "WYDOBYCIE", "ДОБЫЧА", "ВИДОБУТОК",
     "MADENCİLİK", "採掘", "채광", "采矿", "採礦", "การขุด"},
    // CatMaterials
    {"MATERIALS", "MATERIALIEN", "MATÉRIAUX", "MATERIALI", "MATERIALES", "MATERIAIS", "MATERIAŁY", "МАТЕРИАЛЫ",
     "МАТЕРІАЛИ", "MALZEMELER", "素材", "재료", "材料", "材料", "วัสดุ"},
    // CatPlants
    {"PLANTS", "PFLANZEN", "PLANTES", "PIANTE", "PLANTAS", "PLANTAS", "ROŚLINY", "РАСТЕНИЯ", "РОСЛИНИ", "BİTKİLER",
     "植物", "식물", "植物", "植物", "พืช"},
    // CatBait: fishing bait critters
    {"BAIT", "KÖDER", "APPÂTS", "ESCHE", "CEBOS", "ISCAS", "PRZYNĘTY", "НАЖИВКА", "НАЖИВКА", "YEM", "餌", "미끼",
     "鱼饵", "魚餌", "เหยื่อ"},
    // CatTrees
    {"TREES", "BÄUME", "ARBRES", "ALBERI", "ÁRBOLES", "ÁRVORES", "DRZEWA", "ДЕРЕВЬЯ", "ДЕРЕВА", "AĞAÇLAR", "木",
     "나무", "树木", "樹木", "ต้นไม้"},
    // MinimapBorder
    {"Minimap Border", "Minikarte Rahmen", "Bordure minicarte", "Bordo minimappa", "Borde del minimapa",
     "Borda do minimapa", "Ramka minimapy", "Рамка миникарты", "Рамка мінімапи", "Mini Harita Çerçevesi",
     "ミニマップの枠", "미니맵 테두리", "小地图边框", "小地圖邊框", "ขอบแผนที่ย่อ"},
    // BorderOrnate
    {"Ornate", "Verziert", "Ornée", "Decorato", "Ornamentado", "Ornamentada", "Ozdobna", "Узорная", "Візерункова",
     "Süslü", "装飾", "장식", "华丽", "華麗", "ลวดลาย"},
    // BorderThin
    {"Thin", "Schmal", "Fine", "Sottile", "Fino", "Fina", "Cienka", "Тонкая", "Тонка", "İnce", "細い", "얇게", "细",
     "細", "บาง"},
    // MinimapInfoBar
    {"Minimap Info Bar", "Minikarte Infoleiste", "Barre d'infos minicarte", "Barra info minimappa",
     "Barra de info del minimapa", "Barra de info do minimapa", "Pasek info minimapy", "Инфопанель миникарты",
     "Інфопанель мінімапи", "Mini Harita Bilgi Çubuğu", "ミニマップ情報バー", "미니맵 정보 표시줄", "小地图信息栏",
     "小地圖資訊列", "แถบข้อมูลแผนที่ย่อ"},
    // FilterLocks: the settings row for the Map Key's padlocks
    {"Filter Locks", "Filtersperren", "Verrous de filtre", "Blocchi filtri", "Bloqueos de filtros",
     "Bloqueios de filtro", "Blokady filtrów", "Блокировка фильтров", "Блокування фільтрів", "Filtre kilitleri",
     "フィルターのロック", "필터 잠금", "筛选锁定", "篩選鎖定", "ล็อกตัวกรอง"},
    // RemoveAllLocks
    {"Remove All", "Alle entfernen", "Tout retirer", "Rimuovi tutti", "Quitar todos", "Remover todos",
     "Usuń wszystkie", "Снять все", "Зняти всі", "Tümünü kaldır", "すべて解除", "모두 해제", "全部解除",
     "全部解除", "ปลดทั้งหมด"},
    // HoldForLocked: tooltip of Show All / Hide All
    {"Hold to bypass filter locks", "Halten, um Filtersperren zu umgehen", "Maintenir pour ignorer les verrous",
     "Tieni premuto per ignorare i blocchi", "Mantén pulsado para omitir bloqueos", "Segure para ignorar os bloqueios",
     "Przytrzymaj, aby pominąć blokady", "Удерживайте, чтобы обойти блокировки", "Утримуйте, щоб обійти блокування",
     "Kilitleri atlamak için basılı tutun", "長押しでロックを無視", "길게 눌러 잠금 무시", "长按以忽略锁定",
     "長按以忽略鎖定", "กดค้างเพื่อข้ามการล็อก"},
    // LockExempt: tooltip of a Map Key padlock
    {"Exempt from Show All / Hide All", "Von Alle zeigen / Alle ausblenden ausgenommen",
     "Exclu de Tout afficher / Tout masquer", "Escluso da Mostra tutto / Nascondi tutto",
     "Excluido de Mostrar todo / Ocultar todo", "Excluído de Mostrar tudo / Ocultar tudo",
     "Pomijany przez Pokaż wszystko / Ukryj wszystko", "Не меняется кнопками Показать всё / Скрыть всё",
     "Не змінюється кнопками Показати все / Сховати все", "Hepsini göster / Hepsini gizle bunu atlar",
     "すべて表示 / すべて非表示の対象外", "모두 표시 / 모두 숨기기에서 제외", "不受全部显示 / 全部隐藏影响",
     "不受全部顯示 / 全部隱藏影響", "ไม่ถูกเปลี่ยนโดยแสดงทั้งหมด / ซ่อนทั้งหมด"},
    // CompletedPlaces: settings row (places the game shows with a check mark)
    {"Completed Places", "Erledigte Orte", "Lieux terminés", "Luoghi completati", "Lugares completados",
     "Locais concluídos", "Ukończone miejsca", "Пройденные места", "Пройдені місця", "Tamamlanan yerler",
     "完了した場所", "완료한 장소", "已完成的地点", "已完成的地點", "สถานที่ที่สำเร็จแล้ว"},
    // Show
    {"Show", "Zeigen", "Afficher", "Mostra", "Mostrar", "Mostrar", "Pokaż", "Показать", "Показати", "Göster",
     "表示", "표시", "显示", "顯示", "แสดง"},
    // TimeFormat: settings row, 12-hour (1:00 PM) or 24-hour (13:00) clock
    {"Time Format", "Zeitformat", "Format de l'heure", "Formato ora", "Formato de hora", "Formato de hora",
     "Format czasu", "Формат времени", "Формат часу", "Saat biçimi", "時刻表示", "시간 형식", "时间格式", "時間格式",
     "รูปแบบเวลา"},
    // Advanced: the settings card's last section
    {"Advanced", "Erweitert", "Avancé", "Avanzate", "Avanzado", "Avançado", "Zaawansowane", "Дополнительно",
     "Додатково", "Gelişmiş", "詳細設定", "고급", "高级", "進階", "ขั้นสูง"},
    // ShowPosition: settings row, your position under the minimap (Y is the height)
    {"Position (X, Y, Z)", "Position (X, Y, Z)", "Position (X, Y, Z)", "Posizione (X, Y, Z)", "Posición (X, Y, Z)",
     "Posição (X, Y, Z)", "Pozycja (X, Y, Z)", "Позиция (X, Y, Z)", "Позиція (X, Y, Z)", "Konum (X, Y, Z)",
     "位置 (X, Y, Z)", "위치 (X, Y, Z)", "位置 (X, Y, Z)", "位置 (X, Y, Z)", "ตำแหน่ง (X, Y, Z)"},
    // AllRegions: Map Key > Region, no region picked
    {"All Regions", "Alle Regionen", "Toutes les régions", "Tutte le regioni", "Todas las regiones",
     "Todas as regiões", "Wszystkie regiony", "Все регионы", "Усі регіони", "Tüm bölgeler", "すべての地域", "모든 지역",
     "所有地区", "所有地區", "ทุกภูมิภาค"},
    // Dim: Map Key > Region, pins outside the region drawn faint (the other choice is Hide)
    {"Dim", "Abblenden", "Atténuer", "Attenua", "Atenuar", "Esmaecer", "Przyciemnij", "Приглушить", "Приглушити",
     "Soluklaştır", "薄く表示", "흐리게", "淡化", "淡化", "จาง"},
    // HintCopyPosition (middle click): the World Map hint bar, copies the X and Z under the cursor
    {"Copy X, Z", "X, Z kopieren", "Copier X, Z", "Copia X, Z", "Copiar X, Z", "Copiar X, Z", "Kopiuj X, Z",
     "Копировать X, Z", "Копіювати X, Z", "X, Z kopyala", "X, Z をコピー", "X, Z 복사", "复制 X, Z", "複製 X, Z",
     "คัดลอก X, Z"},
    // RegionFilter: Settings > Advanced, the Map Key's Region row (show one region, the rest dimmed or hidden)
    {"Region Filter", "Regionsfilter", "Filtre de région", "Filtro regione", "Filtro de región", "Filtro de região",
     "Filtr regionu", "Фильтр регионов", "Фільтр регіонів", "Bölge filtresi", "地域フィルター", "지역 필터", "地区筛选",
     "地區篩選", "ตัวกรองภูมิภาค"},
    // RegionColors: Settings > Advanced, each region tinted in its own colour on the World Map
    {"Region Colors", "Regionsfarben", "Couleurs des régions", "Colori delle regioni", "Colores de región",
     "Cores das regiões", "Kolory regionów", "Цвета регионов", "Кольори регіонів", "Bölge renkleri", "地域の色分け",
     "지역 색상", "地区颜色", "地區顏色", "สีภูมิภาค"},
    // TownPortals: Fast Travel & Altars row, the open portals of the Town Portal mod
    {"Town Portals", "Stadtportale", "Portails de ville", "Portali cittadini", "Portales de ciudad",
     "Portais da cidade", "Portale miejskie", "Городские порталы", "Міські портали", "Şehir Portalları",
     "タウンポータル", "마을 포털", "城镇传送门", "城鎮傳送門", "พอร์ทัลเมือง"},
    // HideRowFmt: the button under a marker's right-click menu; %s is its Map Key row
    {"Hide: %s", "Ausblenden: %s", "Masquer : %s", "Nascondi: %s", "Ocultar: %s",
     "Ocultar: %s", "Ukryj: %s", "Скрыть: %s", "Приховати: %s", "Gizle: %s",
     "非表示: %s", "숨기기: %s", "隐藏：%s", "隱藏：%s", "ซ่อน: %s"},
    // HintHideFilter (the map's hint bar, with Alt + left click): turn off the Map Key row of the marker clicked
    {"Hide Filter", "Filter ausblenden", "Masquer le filtre", "Nascondi filtro", "Ocultar filtro",
     "Ocultar filtro", "Ukryj filtr", "Скрыть фильтр", "Приховати фільтр", "Filtreyi gizle",
     "フィルターを隠す", "필터 숨기기", "隐藏筛选", "隱藏篩選", "ซ่อนตัวกรอง"},
    // HintUndoFilter (the map's hint bar, Ctrl+Z while there is a filter change to undo)
    {"Undo Filter", "Filter rückgängig", "Annuler filtre", "Annulla filtro", "Deshacer filtro",
     "Desfazer filtro", "Cofnij filtr", "Отменить фильтр", "Скасувати фільтр", "Filtreyi geri al",
     "フィルターを元に戻す", "필터 실행 취소", "撤销筛选", "復原篩選", "เลิกทำตัวกรอง"},
    // HintRedoFilter (the map's hint bar, Ctrl+Y while there is an undone change to redo)
    {"Redo Filter", "Filter wiederholen", "Rétablir filtre", "Ripeti filtro", "Rehacer filtro",
     "Refazer filtro", "Ponów filtr", "Вернуть фильтр", "Повторити фільтр", "Filtreyi yinele",
     "フィルターをやり直す", "필터 다시 실행", "重做筛选", "重做篩選", "ทำตัวกรองซ้ำ"},
    // SearchList: the Map Key's search box (under Show All / Hide All); narrows the panel's rows, not the map
    {"Search this list", "Diese Liste durchsuchen", "Rechercher dans cette liste", "Cerca in questo elenco",
     "Buscar en esta lista", "Pesquisar nesta lista", "Szukaj na tej liście", "Поиск по списку", "Пошук у списку",
     "Bu listede ara", "このリストを検索", "이 목록 검색", "搜索此列表", "搜尋此清單", "ค้นหาในรายการนี้"},
    // NoListMatches: the Map Key's search found no row
    {"No matching rows", "Keine passenden Zeilen", "Aucune ligne correspondante", "Nessuna riga corrispondente",
     "Ninguna fila coincide", "Nenhuma linha corresponde", "Brak pasujących wierszy", "Нет подходящих строк",
     "Немає відповідних рядків", "Eşleşen satır yok", "一致する行はありません", "일치하는 항목 없음", "没有匹配的行",
     "沒有符合的列", "ไม่มีแถวที่ตรงกัน"},
};
// clang-format on

static_assert(sizeof(kTable) / sizeof(kTable[0]) == static_cast<size_t>(Str::Count),
              "one row per Str, in order");

// Resource row names (ore_deposits.h kOreLabels, same order), for when the game's own item name can't be
// used: a row with no single item (trees), or a language set in the ini that isn't the game's.
// clang-format off
const Row kResourceNames[] = {
    {"Flintstone", "Feuerstein", "Silex", "Selce", "Pedernal", "Sílex", "Krzemień", "Кремень", "Кремінь", "Çakmaktaşı", "火打ち石", "부싯돌", "燧石", "燧石", "หินเหล็กไฟ"},
    {"Salt", "Salz", "Sel", "Sale", "Sal", "Sal", "Sól", "Соль", "Сіль", "Tuz", "塩", "소금", "盐", "鹽", "เกลือ"},
    {"Copper", "Kupfer", "Cuivre", "Rame", "Cobre", "Cobre", "Miedź", "Медь", "Мідь", "Bakır", "銅", "구리", "铜", "銅", "ทองแดง"},
    {"Clay", "Lehm", "Argile", "Argilla", "Arcilla", "Argila", "Glina", "Глина", "Глина", "Kil", "粘土", "점토", "黏土", "黏土", "ดินเหนียว"},
    {"Amber", "Bernstein", "Ambre", "Ambra", "Ámbar", "Âmbar", "Bursztyn", "Янтарь", "Бурштин", "Kehribar", "琥珀", "호박", "琥珀", "琥珀", "อำพัน"},
    {"Tin", "Zinn", "Étain", "Stagno", "Estaño", "Estanho", "Cyna", "Олово", "Олово", "Kalay", "錫", "주석", "锡", "錫", "ดีบุก"},
    {"Limestone", "Kalkstein", "Calcaire", "Calcare", "Caliza", "Calcário", "Wapień", "Известняк", "Вапняк", "Kireçtaşı", "石灰岩", "석회암", "石灰石", "石灰石", "หินปูน"},
    {"Fossilized Bone", "Versteinerter Knochen", "Os fossilisé", "Osso fossile", "Hueso fosilizado", "Osso fossilizado", "Skamieniała kość", "Окаменелая кость", "Скам'яніла кістка", "Fosilleşmiş kemik", "骨の化石", "화석화된 뼈", "骨化石", "骨化石", "กระดูกฟอสซิล"},
    {"Sulfur", "Schwefel", "Soufre", "Zolfo", "Azufre", "Enxofre", "Siarka", "Сера", "Сірка", "Kükürt", "硫黄", "유황", "硫磺", "硫磺", "กำมะถัน"},
    {"Iron", "Eisen", "Fer", "Ferro", "Hierro", "Ferro", "Żelazo", "Железо", "Залізо", "Demir", "鉄", "철", "铁", "鐵", "เหล็ก"},
    {"Lapis Lazuli", "Lapislazuli", "Lapis-lazuli", "Lapislazzuli", "Lapislázuli", "Lápis-lazúli", "Lazuryt", "Лазурит", "Лазурит", "Lacivert taşı", "ラピスラズリ", "청금석", "青金石", "青金石", "ลาพิสลาซูลี"},
    {"Coal", "Kohle", "Charbon", "Carbone", "Carbón", "Carvão", "Węgiel", "Уголь", "Вугілля", "Kömür", "石炭", "석탄", "煤炭", "煤炭", "ถ่านหิน"},
    {"Silver", "Silber", "Argent", "Argento", "Plata", "Prata", "Srebro", "Серебро", "Срібло", "Gümüş", "銀", "은", "银", "銀", "เงิน"},
    {"Amethyst", "Amethyst", "Améthyste", "Ametista", "Amatista", "Ametista", "Ametyst", "Аметист", "Аметист", "Ametist", "アメジスト", "자수정", "紫水晶", "紫水晶", "อเมทิสต์"},
    {"Obsidian", "Obsidian", "Obsidienne", "Ossidiana", "Obsidiana", "Obsidiana", "Obsydian", "Обсидиан", "Обсидіан", "Obsidyen", "黒曜石", "흑요석", "黑曜石", "黑曜石", "ออบซิเดียน"},
    {"Gold", "Gold", "Or", "Oro", "Oro", "Ouro", "Złoto", "Золото", "Золото", "Altın", "金", "금", "金", "金", "ทองคำ"},
    {"Aquamarine", "Aquamarin", "Aigue-marine", "Acquamarina", "Aguamarina", "Água-marinha", "Akwamaryn", "Аквамарин", "Аквамарин", "Akuamarin", "アクアマリン", "아쿠아마린", "海蓝宝石", "海藍寶石", "อะความารีน"},
    {"Mother of Pearl", "Perlmutt", "Nacre", "Madreperla", "Nácar", "Madrepérola", "Masa perłowa", "Перламутр", "Перламутр", "Sedef", "真珠層", "자개", "珍珠母", "珍珠母", "มุก"},
    {"Red Marble", "Roter Marmor", "Marbre rouge", "Marmo rosso", "Mármol rojo", "Mármore vermelho", "Czerwony marmur", "Красный мрамор", "Червоний мармур", "Kırmızı mermer", "赤大理石", "붉은 대리석", "红色大理石", "紅色大理石", "หินอ่อนแดง"},
    {"Diamond", "Diamant", "Diamant", "Diamante", "Diamante", "Diamante", "Diament", "Алмаз", "Алмаз", "Elmas", "ダイヤモンド", "다이아몬드", "钻石", "鑽石", "เพชร"},
    {"Celesthium", "Celesthium", "Célesthium", "Celesthium", "Celesthium", "Celesthium", "Celesthium", "Целестиум", "Целестіум", "Celesthium", "セレスティウム", "셀레스티움", "Celesthium", "Celesthium", "Celesthium"},
    {"Farm Soil", "Ackerboden", "Terre agricole", "Terra coltivabile", "Tierra de cultivo", "Solo agrícola", "Ziemia uprawna", "Пахотная земля", "Орна земля", "Tarım toprağı", "畑の土", "농토", "农田土", "農田土", "ดินเพาะปลูก"},
    {"Fertilised Farm Soil", "Gedüngter Ackerboden", "Terre agricole fertilisée", "Terra coltivabile fertilizzata", "Tierra de cultivo abonada", "Solo agrícola fertilizado", "Nawożona ziemia uprawna", "Удобренная пахотная земля", "Удобрена орна земля", "Gübrelenmiş tarım toprağı", "肥えた畑の土", "비옥한 농토", "施肥农田土", "施肥農田土", "ดินเพาะปลูกใส่ปุ๋ย"},
    {"Flower Soil", "Blumenerde", "Terreau", "Terriccio", "Tierra para flores", "Terra para flores", "Ziemia kwiatowa", "Цветочная земля", "Квіткова земля", "Çiçek toprağı", "花の土", "꽃 흙", "花土", "花土", "ดินปลูกดอกไม้"},
    {"Twigs", "Zweige", "Brindilles", "Ramoscelli", "Ramitas", "Gravetos", "Gałązki", "Ветки", "Гілки", "Dal parçaları", "小枝", "잔가지", "细枝", "細枝", "กิ่งไม้"},
    {"Bones", "Knochen", "Os", "Ossa", "Huesos", "Ossos", "Kości", "Кости", "Кістки", "Kemikler", "骨", "뼈", "骨头", "骨頭", "กระดูก"},
    {"Tar", "Teer", "Goudron", "Catrame", "Alquitrán", "Piche", "Smoła", "Смола", "Смола", "Katran", "タール", "타르", "焦油", "焦油", "น้ำมันดิน"},
    {"Toxic Slime", "Giftschleim", "Bave toxique", "Melma tossica", "Baba tóxica", "Gosma tóxica", "Toksyczny śluz", "Ядовитая слизь", "Отруйний слиз", "Zehirli balçık", "毒スライム", "독성 점액", "毒性黏液", "毒性黏液", "เมือกพิษ"},
    {"Luminous Growth", "Leuchtgewächs", "Excroissance lumineuse", "Escrescenza luminosa", "Excrecencia luminosa", "Crescimento luminoso", "Świecąca narośl", "Светящийся нарост", "Світний наріст", "Parlayan yumru", "発光体", "빛나는 덩어리", "发光菌丛", "發光菌叢", "สิ่งเรืองแสง"},
    {"Underwater Luminous Growth", "Unterwasser-Leuchtgewächs", "Excroissance lumineuse sous-marine", "Escrescenza luminosa subacquea", "Excrecencia luminosa submarina", "Crescimento luminoso subaquático", "Podwodna świecąca narośl", "Подводный светящийся нарост", "Підводний світний наріст", "Sualtı parlayan yumru", "水中の発光体", "수중 빛나는 덩어리", "水下发光菌丛", "水下發光菌叢", "สิ่งเรืองแสงใต้น้ำ"},
    {"Ice", "Eis", "Glace", "Ghiaccio", "Hielo", "Gelo", "Lód", "Лёд", "Лід", "Buz", "氷", "얼음", "冰", "冰", "น้ำแข็ง"},
    {"Gold Coins", "Goldmünzen", "Pièces d'or", "Monete d'oro", "Monedas de oro", "Moedas de ouro", "Złote monety", "Золотые монеты", "Золоті монети", "Altın paralar", "金貨", "금화", "金币", "金幣", "เหรียญทอง"},
    {"Archaic Essence", "Archaische Essenz", "Essence archaïque", "Essenza arcaica", "Esencia arcaica", "Essência arcaica", "Archaiczna esencja", "Древняя эссенция", "Давня есенція", "Kadim öz", "古代のエッセンス", "고대의 정수", "远古精华", "遠古精華", "แก่นโบราณ"},
    {"Corncob", "Maiskolben", "Épi de maïs", "Pannocchia", "Mazorca", "Espiga de milho", "Kolba kukurydzy", "Початок кукурузы", "Качан кукурудзи", "Mısır koçanı", "トウモロコシ", "옥수수", "玉米", "玉米", "ฝักข้าวโพด"},
    {"Aureolin Flower", "Aureolinblume", "Fleur d'auréoline", "Fiore di aureolina", "Flor de aureolina", "Flor de aureolina", "Kwiat aureoliny", "Ауреолиновый цветок", "Квітка ауреоліну", "Aureolin çiçeği", "オーレオリンの花", "오레올린 꽃", "金黄花", "金黃花", "ดอกออเรโอลิน"},
    {"Strawberry", "Erdbeere", "Fraise", "Fragola", "Fresa", "Morango", "Truskawka", "Клубника", "Полуниця", "Çilek", "イチゴ", "딸기", "草莓", "草莓", "สตรอว์เบอร์รี"},
    {"Forest Beet", "Waldrübe", "Betterave des bois", "Barbabietola selvatica", "Remolacha silvestre", "Beterraba silvestre", "Burak leśny", "Лесная свёкла", "Лісовий буряк", "Orman pancarı", "森のビート", "숲 비트", "森林甜菜", "森林甜菜", "บีทรูทป่า"},
    {"Flax", "Flachs", "Lin", "Lino", "Lino", "Linho", "Len", "Лён", "Льон", "Keten", "亜麻", "아마", "亚麻", "亞麻", "ป่าน"},
    {"Chamomile", "Kamille", "Camomille", "Camomilla", "Manzanilla", "Camomila", "Rumianek", "Ромашка", "Ромашка", "Papatya", "カモミール", "캐모마일", "洋甘菊", "洋甘菊", "คาโมมายล์"},
    {"Stinging Nettle", "Brennnessel", "Ortie", "Ortica", "Ortiga", "Urtiga", "Pokrzywa", "Крапива", "Кропива", "Isırgan otu", "イラクサ", "쐐기풀", "荨麻", "蕁麻", "ตำแย"},
    {"Indigo Plant", "Indigopflanze", "Indigotier", "Pianta d'indaco", "Planta de índigo", "Anileira", "Indygowiec", "Индигоноска", "Індигоноска", "Çivit bitkisi", "藍", "쪽", "木蓝", "木藍", "ต้นคราม"},
    {"Hazelnut", "Haselnuss", "Noisette", "Nocciola", "Avellana", "Avelã", "Orzech laskowy", "Лесной орех", "Лісовий горіх", "Fındık", "ヘーゼルナッツ", "헤이즐넛", "榛子", "榛果", "เฮเซลนัท"},
    {"Rooibos", "Rooibos", "Rooibos", "Rooibos", "Rooibos", "Rooibos", "Rooibos", "Ройбуш", "Ройбуш", "Rooibos", "ルイボス", "루이보스", "路易波士", "路易波士", "รอยบอส"},
    {"Bell Pepper", "Paprika", "Poivron", "Peperone", "Pimiento", "Pimentão", "Papryka", "Сладкий перец", "Солодкий перець", "Dolmalık biber", "ピーマン", "피망", "甜椒", "甜椒", "พริกหยวก"},
    {"Sage Leaves", "Salbeiblätter", "Feuilles de sauge", "Foglie di salvia", "Hojas de salvia", "Folhas de sálvia", "Liście szałwii", "Листья шалфея", "Листя шавлії", "Adaçayı yaprakları", "セージの葉", "세이지 잎", "鼠尾草叶", "鼠尾草葉", "ใบเสจ"},
    {"Sugar Cane", "Zuckerrohr", "Canne à sucre", "Canna da zucchero", "Caña de azúcar", "Cana-de-açúcar", "Trzcina cukrowa", "Сахарный тростник", "Цукрова тростина", "Şeker kamışı", "サトウキビ", "사탕수수", "甘蔗", "甘蔗", "อ้อย"},
    {"Aloe", "Aloe", "Aloès", "Aloe", "Aloe", "Babosa", "Aloes", "Алоэ", "Алое", "Aloe", "アロエ", "알로에", "芦荟", "蘆薈", "ว่านหางจระเข้"},
    {"Saffron", "Safran", "Safran", "Zafferano", "Azafrán", "Açafrão", "Szafran", "Шафран", "Шафран", "Safran", "サフラン", "사프란", "藏红花", "番紅花", "หญ้าฝรั่น"},
    {"Grapple Plant", "Greifpflanze", "Plante grappin", "Pianta rampino", "Planta garfio", "Planta-gancho", "Roślina z hakiem", "Растение-крюк", "Рослина-гак", "Kanca bitkisi", "グラップル植物", "갈고리 식물", "钩爪植物", "鉤爪植物", "พืชตะขอ"},
    {"Shadowbloom", "Schattenblüte", "Fleur d'ombre", "Fiore d'ombra", "Flor de sombra", "Flor-sombria", "Cienisty kwiat", "Теневой цветок", "Тіньовий квіт", "Gölge çiçeği", "影の花", "그림자꽃", "暗影花", "暗影花", "ดอกเงา"},
    {"Wolfsbane", "Eisenhut", "Aconit", "Aconito", "Acónito", "Acônito", "Tojad", "Аконит", "Аконіт", "Kurtboğan", "トリカブト", "투구꽃", "乌头", "烏頭", "อะโคไนต์"},
    {"Gentian", "Enzian", "Gentiane", "Genziana", "Genciana", "Genciana", "Goryczka", "Горечавка", "Тирлич", "Centiyan", "リンドウ", "용담", "龙胆", "龍膽", "เจนเชียน"},
    {"Taro", "Taro", "Taro", "Taro", "Taro", "Inhame", "Taro", "Таро", "Таро", "Gölevez", "タロイモ", "토란", "芋头", "芋頭", "เผือก"},
    {"Ginger", "Ingwer", "Gingembre", "Zenzero", "Jengibre", "Gengibre", "Imbir", "Имбирь", "Імбир", "Zencefil", "ショウガ", "생강", "生姜", "生薑", "ขิง"},
    {"Shiitake Mushroom", "Shiitake-Pilz", "Champignon shiitaké", "Fungo shiitake", "Seta shiitake", "Cogumelo shiitake", "Grzyb shiitake", "Гриб шиитаке", "Гриб шиїтаке", "Şitake mantarı", "シイタケ", "표고버섯", "香菇", "香菇", "เห็ดหอม"},
    {"Papyrus", "Papyrus", "Papyrus", "Papiro", "Papiro", "Papiro", "Papirus", "Папирус", "Папірус", "Papirüs", "パピルス", "파피루스", "纸莎草", "紙莎草", "ต้นปาปิรัส"},
    {"Algae", "Algen", "Algues", "Alghe", "Algas", "Algas", "Glony", "Водоросли", "Водорості", "Yosun", "藻", "해조류", "藻类", "藻類", "สาหร่าย"},
    {"Daylily", "Taglilie", "Hémérocalle", "Emerocallide", "Lirio de día", "Lírio-de-um-dia", "Liliowiec", "Лилейник", "Лілійник", "Gün zambağı", "デイリリー", "원추리", "萱草", "萱草", "เดย์ลิลลี่"},
    {"Thyme", "Thymian", "Thym", "Timo", "Tomillo", "Tomilho", "Tymianek", "Тимьян", "Чебрець", "Kekik", "タイム", "타임", "百里香", "百里香", "ไธม์"},
    {"Aurora Beans", "Aurorabohnen", "Haricots aurore", "Fagioli aurora", "Judías aurora", "Feijões aurora", "Fasola zorzy", "Бобы авроры", "Боби аврори", "Aurora fasulyesi", "オーロラ豆", "오로라 콩", "极光豆", "極光豆", "ถั่วออโรรา"},
    {"Prickle Pearl", "Stachelperle", "Perle épineuse", "Perla spinosa", "Perla espinosa", "Pérola espinhosa", "Kolczasta perła", "Колючая жемчужина", "Колюча перлина", "Dikenli inci", "トゲ真珠", "가시 진주", "刺珍珠", "刺珍珠", "ไข่มุกหนาม"},
    {"Grandleaf Cabbage", "Großblattkohl", "Chou à grandes feuilles", "Cavolo a foglia grande", "Col de hoja grande", "Repolho de folha grande", "Kapusta wielkolistna", "Крупнолистная капуста", "Великолиста капуста", "Büyük yapraklı lahana", "大葉キャベツ", "큰잎 양배추", "大叶卷心菜", "大葉高麗菜", "กะหล่ำใบใหญ่"},
    {"Bloomheart Fruit", "Blütenherzfrucht", "Fruit coeur-de-fleur", "Frutto cuorfiore", "Fruta corazón de flor", "Fruta coração-de-flor", "Owoc kwietnego serca", "Плод цветочного сердца", "Плід квіткового серця", "Çiçek kalbi meyvesi", "ブルームハートの実", "블룸하트 열매", "花心果", "花心果", "ผลบลูมฮาร์ท"},
    {"Fire Fly", "Glühwürmchen", "Luciole", "Lucciola", "Luciérnaga", "Vaga-lume", "Świetlik", "Светлячок", "Світлячок", "Ateş böceği", "ホタル", "반딧불이", "萤火虫", "螢火蟲", "หิ่งห้อย"},
    {"Grasshoppers", "Grashüpfer", "Sauterelles", "Cavallette", "Saltamontes", "Gafanhotos", "Koniki polne", "Кузнечики", "Коники", "Çekirgeler", "バッタ", "메뚜기", "蚱蜢", "蚱蜢", "ตั๊กแตน"},
    {"Scarab", "Skarabäus", "Scarabée", "Scarabeo", "Escarabajo", "Escaravelho", "Skarabeusz", "Скарабей", "Скарабей", "Bok böceği", "スカラベ", "스카라베", "圣甲虫", "聖甲蟲", "แมลงสคารับ"},
    {"Pillbug", "Rollassel", "Cloporte", "Porcellino di terra", "Cochinilla", "Tatuzinho", "Prosionek", "Мокрица", "Мокриця", "Tespih böceği", "ダンゴムシ", "공벌레", "鼠妇", "鼠婦", "แมลงม้วนตัว"},
    {"Frog", "Frosch", "Grenouille", "Rana", "Rana", "Rã", "Żaba", "Лягушка", "Жаба", "Kurbağa", "カエル", "개구리", "青蛙", "青蛙", "กบ"},
    {"Maggots", "Maden", "Asticots", "Larve", "Gusanos", "Larvas", "Larwy", "Личинки", "Личинки", "Kurtçuklar", "ウジ", "구더기", "蛆", "蛆", "หนอนแมลงวัน"},
    {"Springlands Trees", "Frühlingsland-Bäume", "Arbres des Terres printanières", "Alberi delle Terre Primaverili", "Árboles de las Tierras Primaverales", "Árvores das Terras Primaveris", "Drzewa Wiosennych Ziem", "Деревья Весенних земель", "Дерева Весняних земель", "Bahar Toprakları ağaçları", "春の地の木", "봄의 땅 나무", "春之地树木", "春之地樹木", "ต้นไม้ดินแดนวสันต์"},
    {"Resin Trees", "Harzbäume", "Arbres à résine", "Alberi da resina", "Árboles de resina", "Árvores de resina", "Drzewa żywiczne", "Смолистые деревья", "Смолисті дерева", "Reçine ağaçları", "樹脂の木", "수지 나무", "树脂树", "樹脂樹", "ต้นยางไม้"},
    {"Hornbeam", "Hainbuche", "Charme", "Carpino", "Carpe", "Carpino", "Grab", "Граб", "Граб", "Gürgen", "シデ", "서어나무", "鹅耳枥", "鵝耳櫪", "ต้นฮอร์นบีม"},
    {"Revelwood Trees", "Revelwood-Bäume", "Arbres de Revelwood", "Alberi di Revelwood", "Árboles de Revelwood", "Árvores de Revelwood", "Drzewa Revelwood", "Деревья Ревелвуда", "Дерева Ревелвуду", "Revelwood ağaçları", "レベルウッドの木", "레벨우드 나무", "Revelwood 树木", "Revelwood 樹木", "ต้นไม้เรเวลวูด"},
    {"Giant Trees", "Riesenbäume", "Arbres géants", "Alberi giganti", "Árboles gigantes", "Árvores gigantes", "Olbrzymie drzewa", "Гигантские деревья", "Гігантські дерева", "Dev ağaçlar", "巨木", "거대한 나무", "巨树", "巨樹", "ต้นไม้ยักษ์"},
    {"Highlands Trees", "Hochlandbäume", "Arbres des Hautes-Terres", "Alberi degli Altopiani", "Árboles de las Tierras Altas", "Árvores das Terras Altas", "Drzewa wyżynne", "Деревья нагорья", "Дерева нагір'я", "Yayla ağaçları", "高地の木", "고지대 나무", "高地树木", "高地樹木", "ต้นไม้ที่ราบสูง"},
    {"Date Palms", "Dattelpalmen", "Palmiers dattiers", "Palme da dattero", "Palmeras datileras", "Tamareiras", "Palmy daktylowe", "Финиковые пальмы", "Фінікові пальми", "Hurma ağaçları", "ナツメヤシ", "대추야자", "椰枣树", "椰棗樹", "ต้นอินทผลัม"},
    {"Albaneve Trees", "Albaneve-Bäume", "Arbres d'Albaneve", "Alberi di Albaneve", "Árboles de Albaneve", "Árvores de Albaneve", "Drzewa Albaneve", "Деревья Албанева", "Дерева Албанева", "Albaneve ağaçları", "アルバネーベの木", "알바네베 나무", "Albaneve 树木", "Albaneve 樹木", "ต้นไม้อัลบาเนเว"},
    {"Juniper", "Wacholder", "Genévrier", "Ginepro", "Enebro", "Zimbro", "Jałowiec", "Можжевельник", "Ялівець", "Ardıç", "ネズ", "노간주나무", "杜松", "杜松", "จูนิเปอร์"},
    {"Mountain Pine", "Bergkiefer", "Pin de montagne", "Pino mugo", "Pino de montaña", "Pinheiro-da-montanha", "Kosodrzewina", "Горная сосна", "Гірська сосна", "Dağ çamı", "ハイマツ", "산소나무", "山松", "山松", "สนภูเขา"},
    {"Birch", "Birke", "Bouleau", "Betulla", "Abedul", "Bétula", "Brzoza", "Берёза", "Береза", "Huş", "シラカバ", "자작나무", "桦树", "樺樹", "ต้นเบิร์ช"},
    {"Deadwood", "Totholz", "Bois mort", "Legno morto", "Madera muerta", "Madeira morta", "Martwe drewno", "Сухостой", "Сухостій", "Ölü ağaç", "枯れ木", "고사목", "枯木", "枯木", "ไม้ตาย"},
    {"Veilwater Trees", "Veilwater-Bäume", "Arbres de Veilwater", "Alberi di Veilwater", "Árboles de Veilwater", "Árvores de Veilwater", "Drzewa Veilwater", "Деревья Вейлвотера", "Дерева Вейлвотера", "Veilwater ağaçları", "ヴェイルウォーターの木", "베일워터 나무", "Veilwater 树木", "Veilwater 樹木", "ต้นไม้เวลวอเทอร์"},
    {"Mangrove", "Mangroven", "Palétuviers", "Mangrovie", "Manglares", "Mangues", "Namorzyny", "Мангровые деревья", "Мангрові дерева", "Mangrov", "マングローブ", "맹그로브", "红树", "紅樹", "ต้นโกงกาง"},
    {"Coconut Palms", "Kokospalmen", "Cocotiers", "Palme da cocco", "Cocoteros", "Coqueiros", "Palmy kokosowe", "Кокосовые пальмы", "Кокосові пальми", "Hindistan cevizi ağaçları", "ココヤシ", "코코넛 야자", "椰子树", "椰子樹", "ต้นมะพร้าว"},
    {"Licuala Palms", "Licuala-Palmen", "Palmiers licuala", "Palme licuala", "Palmeras licuala", "Palmeiras licuala", "Palmy licuala", "Пальмы ликуала", "Пальми лікуала", "Licuala palmiyeleri", "リクアラヤシ", "리쿠알라 야자", "圆叶轴榈", "圓葉軸櫚", "ปาล์มพัด"},
    {"Bamboo", "Bambus", "Bambou", "Bambù", "Bambú", "Bambu", "Bambus", "Бамбук", "Бамбук", "Bambu", "竹", "대나무", "竹子", "竹子", "ไผ่"},
    {"Coffee Trees", "Kaffeebäume", "Caféiers", "Piante di caffè", "Cafetos", "Cafeeiros", "Kawowce", "Кофейные деревья", "Кавові дерева", "Kahve ağaçları", "コーヒーの木", "커피나무", "咖啡树", "咖啡樹", "ต้นกาแฟ"},
    {"Return Beacon", "Rückkehrleuchtfeuer", "Balise de retour", "Faro del ritorno", "Baliza de retorno", "Farol de retorno", "Latarnia powrotu", "Маяк возвращения", "Маяк повернення", "Dönüş işareti", "帰還のビーコン", "귀환 신호기", "返回信标", "返回信標", "สัญญาณกลับ"},
    {"Water Well", "Brunnen", "Puits", "Pozzo", "Pozo", "Poço", "Studnia", "Колодец", "Криниця", "Kuyu", "井戸", "우물", "水井", "水井", "บ่อน้ำ"},
    {"Water Barrel", "Wasserfass", "Tonneau d'eau", "Barile d'acqua", "Barril de agua", "Barril de água", "Beczka z wodą", "Бочка с водой", "Бочка з водою", "Su fıçısı", "水樽", "물통", "水桶", "水桶", "ถังน้ำ"},
    {"Lore Note", "Notiz", "Note", "Nota", "Nota", "Nota", "Notatka", "Записка", "Записка", "Not", "メモ", "쪽지", "笔记", "筆記", "บันทึก"},
    {"Boss", "Boss", "Boss", "Boss", "Jefe", "Chefe", "Boss", "Босс", "Бос", "Boss", "ボス", "보스", "首领", "首領", "บอส"},
    {"Unique Weapons", "Einzigartige Waffen", "Armes uniques", "Armi uniche", "Armas únicas", "Armas únicas", "Unikatowa broń", "Уникальное оружие", "Унікальна зброя", "Eşsiz silahlar", "ユニーク武器", "고유 무기", "独特武器", "獨特武器", "อาวุธพิเศษ"},
    {"Unique Equipment", "Einzigartige Ausrüstung", "Équipement unique", "Equipaggiamento unico", "Equipo único", "Equipamento único", "Unikatowy ekwipunek", "Уникальное снаряжение", "Унікальне спорядження", "Eşsiz ekipman", "ユニーク装備", "고유 장비", "独特装备", "獨特裝備", "อุปกรณ์พิเศษ"},
    {"Collectibles", "Sammlerstücke", "Objets de collection", "Collezionabili", "Coleccionables", "Colecionáveis", "Przedmioty kolekcjonerskie", "Коллекционные предметы", "Колекційні предмети", "Koleksiyon parçaları", "コレクション品", "수집품", "收藏品", "收藏品", "ของสะสม"},
    {"Fell Summoning Stone", "Fell-Beschwörungsstein", "Pierre d'invocation de Fell", "Pietra di evocazione Fell", "Piedra de invocación Fell", "Pedra de invocação Fell", "Kamień przywołania Fell", "Камень призыва Fell", "Камінь виклику Fell", "Fell çağırma taşı", "フェル召喚石", "펠 소환석", "邪物召唤石", "邪物召喚石", "หินอัญเชิญเฟลล์"},
    {"Gloomberry", "Düsterbeere", "Baie lugubre", "Bacca cupa", "Baya lúgubre", "Baga sombria", "Mroczna jagoda", "Мрачная ягода", "Похмура ягода", "Kasvet meyvesi", "グルームベリー", "음울한 열매", "幽暗浆果", "幽暗漿果", "เบอร์รีมืดมน"},
    {"Midnight Lily", "Mitternachtslilie", "Lys de minuit", "Giglio di mezzanotte", "Lirio de medianoche", "Lírio da meia-noite", "Lilia północy", "Полуночная лилия", "Опівнічна лілія", "Gece yarısı zambağı", "真夜中の百合", "한밤의 백합", "午夜百合", "午夜百合", "ลิลลี่เที่ยงคืน"},
    {"Calla Luna", "Calla Luna", "Calla Luna", "Calla Luna", "Calla Luna", "Calla Luna", "Calla Luna", "Калла Луна", "Калла Луна", "Calla Luna", "カラ・ルナ", "칼라 루나", "月光马蹄莲", "月光馬蹄蓮", "คาลลาลูนา"},
    {"Glimmer Gentian", "Schimmerenzian", "Gentiane scintillante", "Genziana luccicante", "Genciana destellante", "Genciana cintilante", "Migoczący goryczka", "Мерцающая горечавка", "Мерехтлива тирлич", "Pırıltılı centiyane", "きらめくリンドウ", "반짝이는 용담", "微光龙胆", "微光龍膽", "เจนเชียนระยิบระยับ"},
    {"Stapelia Starflower", "Stapelia-Sternblume", "Stapélie étoilée", "Stapelia stellata", "Estapelia estrellada", "Estapélia estrelada", "Stapelia gwiaździsta", "Звёздная стапелия", "Зоряна стапелія", "Yıldız stapelya", "スタペリアの星花", "스타펠리아 별꽃", "星花犀角", "星花犀角", "สตาเพเลียดาว"},
    {"Passionflower", "Passionsblume", "Passiflore", "Passiflora", "Pasionaria", "Maracujá-flor", "Męczennica", "Пассифлора", "Пасифлора", "Çarkıfelek", "トケイソウ", "시계꽃", "西番莲", "西番蓮", "เสาวรส"},
    {"Gleam Root", "Glanzwurzel", "Racine lumineuse", "Radice lucente", "Raíz brillante", "Raiz cintilante", "Lśniący korzeń", "Сияющий корень", "Сяючий корінь", "Parıltı kökü", "グリームルート", "빛나는 뿌리", "微光根", "微光根", "รากเรืองแสง"},
};
// clang-format on

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

bool GameTextInCurrentLanguage() {
    if (g_followsGame) return true;
    static int known = -1; // -1 not asked yet (Steam not up), else 0 / 1
    if (known < 0) {
        const char* steam = SteamGameLanguage();
        if (!steam) return false;
        Lang game{};
        known = (LangFromCode(steam, game) ? game : Lang::English) == g_lang ? 1 : 0;
    }
    return known == 1;
}

size_t ResourceNameCount() { return sizeof(kResourceNames) / sizeof(kResourceNames[0]); }

const char* ResourceName(size_t index) {
    if (index >= ResourceNameCount()) return nullptr;
    const char* text = kResourceNames[index][static_cast<int>(g_lang)];
    return (text && *text) ? text : kResourceNames[index][0];
}

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

int g_clockFormat = -1;
void SetClockFormat(int format) { g_clockFormat = format; }
bool Uses12HourClock() { return g_clockFormat < 0 ? g_lang == Lang::English : g_clockFormat == 0; }

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
