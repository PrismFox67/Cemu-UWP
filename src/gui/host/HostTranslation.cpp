#include "gui/host/HostTranslation.h"

#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"

#include <fstream>
#include <mutex>
#include <unordered_map>

namespace
{
	struct Entry
	{
		const char* english;
		const char* pt; // Portuguese (Brazil)
		const char* es;
	};

	// format specifiers (%s, %d) must stay in the translations
	const Entry kTable[] = {
		// category bar and hints
		{ "Games", "Jogos", "Juegos" },
		{ "Graphic packs", "Pacotes gráficos", "Paquetes gráficos" },
		{ "Controllers", "Controles", "Mandos" },
		{ "Settings", "Configurações", "Ajustes" },
		{ "About", "Sobre", "Acerca de" },
		{ "Start", "Iniciar", "Iniciar" },
		{ "Refresh", "Atualizar", "Actualizar" },
		{ "In-Game Menu", "Menu do jogo", "Menú del juego" },
		{ "Select", "Selecionar", "Seleccionar" },
		{ "Back", "Voltar", "Atrás" },
		{ "Switch", "Trocar", "Cambiar" },
		// games
		{ "No games found.", "Nenhum jogo encontrado.", "No se encontraron juegos." },
		{ "Scanning game folders...", "Procurando jogos nas pastas...", "Buscando juegos en las carpetas..." },
		{ "Open file...", "Abrir arquivo...", "Abrir archivo..." },
		{ "Add a folder that contains your games (WUD, WUX, WUA, RPX or extracted folders) under Settings, or install titles into the mlc folder:",
			"Adicione uma pasta com seus jogos (WUD, WUX, WUA, RPX ou pastas extraídas) em Configurações, ou instale os títulos na pasta mlc:",
			"Añade una carpeta con tus juegos (WUD, WUX, WUA, RPX o carpetas extraídas) en Ajustes, o instala los títulos en la carpeta mlc:" },
		{ "Region free", "Sem região", "Sin región" },
		{ "Unknown region", "Região desconhecida", "Región desconocida" },
		// setup
		{ "1. Your games", "1. Seus jogos", "1. Tus juegos" },
		{ "2. Disc keys (only for .wud / .wux)", "2. Chaves de disco (só para .wud / .wux)", "2. Claves de disco (solo para .wud / .wux)" },
		{ "3. Community graphic packs", "3. Pacotes gráficos da comunidade", "3. Paquetes gráficos de la comunidad" },
		{ "You can change all of this later under Settings and Graphic packs.", "Você pode mudar tudo isso depois em Configurações e Pacotes gráficos.", "Puedes cambiar todo esto más tarde en Ajustes y Paquetes gráficos." },
		{ "Done", "Concluído", "Listo" },
		{ "Add folder...", "Adicionar pasta...", "Añadir carpeta..." },
		{ "Remove", "Remover", "Quitar" },
		{ "Add path", "Adicionar caminho", "Añadir ruta" },
		{ "or type a path, e.g. E:\\WiiU\\games", "ou digite um caminho, ex. E:\\WiiU\\games", "o escribe una ruta, p. ej. E:\\WiiU\\games" },
		{ "Copies a game, update or DLC folder (the one with code, content and meta in it) into Cemu's mlc folder, where games find their updates and DLC.",
			"Copia uma pasta de jogo, atualização ou DLC (a que tem code, content e meta) para a pasta mlc do Cemu, onde os jogos encontram suas atualizações e DLC.",
			"Copia una carpeta de juego, actualización o DLC (la que contiene code, content y meta) a la carpeta mlc de Cemu, donde los juegos encuentran sus actualizaciones y DLC." },
		// settings
		{ "Game folders", "Pastas de jogos", "Carpetas de juegos" },
		{ "Install updates and DLC", "Instalar atualizações e DLC", "Instalar actualizaciones y DLC" },
		{ "Install update / DLC from folder...", "Instalar atualização / DLC de uma pasta...", "Instalar actualización / DLC desde una carpeta..." },
		{ "Install", "Instalar", "Instalar" },
		{ "Cancel", "Cancelar", "Cancelar" },
		{ "Cancel install", "Cancelar instalação", "Cancelar instalación" },
		{ "Look", "Aparência", "Apariencia" },
		{ "Light panel", "Painel claro", "Panel claro" },
		{ "Dark panel", "Painel escuro", "Panel oscuro" },
		{ "Language and region", "Idioma e região", "Idioma y región" },
		{ "Console language (next game start)", "Idioma do console (no próximo jogo)", "Idioma de la consola (al iniciar el próximo juego)" },
		{ "Console region (next game start)", "Região do console (no próximo jogo)", "Región de la consola (al iniciar el próximo juego)" },
		{ "Interface language", "Idioma da interface", "Idioma de la interfaz" },
		{ "Follow console language", "Seguir o idioma do console", "Seguir el idioma de la consola" },
		{ "Auto (follow the game)", "Automático (segue o jogo)", "Automático (sigue el juego)" },
		{ "Japan", "Japão", "Japón" },
		{ "USA", "EUA", "EE. UU." },
		{ "Europe", "Europa", "Europa" },
		{ "Japanese", "Japonês", "Japonés" },
		{ "English", "Inglês", "Inglés" },
		{ "French", "Francês", "Francés" },
		{ "German", "Alemão", "Alemán" },
		{ "Italian", "Italiano", "Italiano" },
		{ "Spanish", "Espanhol", "Español" },
		{ "Chinese", "Chinês", "Chino" },
		{ "Korean", "Coreano", "Coreano" },
		{ "Dutch", "Holandês", "Neerlandés" },
		{ "Portuguese", "Português", "Portugués" },
		{ "Russian", "Russo", "Ruso" },
		{ "Taiwanese", "Taiwanês", "Taiwanés" },
		{ "Keep the region on Auto unless a game needs another one. Each game's region is shown under the game list.",
			"Deixe a região em Automático, a não ser que um jogo precise de outra. A região de cada jogo aparece abaixo da lista de jogos.",
			"Deja la región en Automático salvo que un juego necesite otra. La región de cada juego se muestra bajo la lista de juegos." },
		{ "Graphics", "Gráficos", "Gráficos" },
		{ "Frame interpolation: 120 FPS from 60 (needs a 120 Hz TV, adds a little lag, can smear)",
			"Interpolação de quadros: 120 FPS a partir de 60 (precisa de TV de 120 Hz, adiciona um pouco de atraso, pode borrar)",
			"Interpolación de fotogramas: 120 FPS a partir de 60 (necesita una TV de 120 Hz, añade algo de retraso, puede emborronar)" },
		{ "Compile shaders asynchronously (less stutter, brief glitches)", "Compilar shaders em segundo plano (menos travadas, falhas breves)", "Compilar shaders en segundo plano (menos tirones, fallos breves)" },
		{ "Compile pipelines while the game loads (longer loading, less stutter)", "Compilar pipelines durante o carregamento (carrega mais devagar, menos travadas)", "Compilar pipelines durante la carga (carga más larga, menos tirones)" },
		{ "Quick start: load menu and race-start pipelines first, the rest in the background",
			"Início rápido: carrega primeiro os pipelines do menu e do começo da corrida, o resto em segundo plano",
			"Inicio rápido: carga primero los pipelines del menú y del inicio de carrera, el resto en segundo plano" },
		{ "New shaders", "Shaders novos", "Shaders nuevos" },
		{ "Smooth: never wait, new effects can pop in", "Suave: nunca espera, efeitos novos podem aparecer de repente", "Fluido: nunca espera, los efectos nuevos pueden aparecer de golpe" },
		{ "Balanced: short waits, rarely misses an effect", "Equilibrado: esperas curtas, raramente perde um efeito", "Equilibrado: esperas cortas, rara vez falta un efecto" },
		{ "Accurate: always wait, stutters", "Preciso: sempre espera, trava", "Preciso: siempre espera, da tirones" },
		{ "Shader compiler (next game start)", "Compilador de shaders (no próximo jogo)", "Compilador de shaders (al iniciar el próximo juego)" },
		{ "Upscale filter", "Filtro de escala", "Filtro de escalado" },
		{ "Scaling", "Escala", "Escalado" },
		{ "Keep aspect ratio", "Manter proporção", "Mantener proporción" },
		{ "Stretch", "Esticar", "Estirar" },
		{ "Show FPS", "Mostrar FPS", "Mostrar FPS" },
		{ "Audio", "Áudio", "Audio" },
		{ "Sound output (applies when a game starts)", "Saída de som (vale ao iniciar um jogo)", "Salida de sonido (se aplica al iniciar un juego)" },
		{ "Stereo", "Estéreo", "Estéreo" },
		{ "Volume", "Volume", "Volumen" },
		{ "Folders", "Pastas", "Carpetas" },
		{ "Import keys.txt from game folders", "Importar keys.txt das pastas de jogos", "Importar keys.txt de las carpetas de juegos" },
		{ "needed for WUD/WUX disc images", "necessário para imagens de disco WUD/WUX", "necesario para imágenes de disco WUD/WUX" },
		{ "Logs and backups", "Logs e backups", "Registros y copias de seguridad" },
		{ "Copies go to %s", "As cópias vão para %s", "Las copias van a %s" },
		{ "Save logs to USB", "Salvar logs no USB", "Guardar registros en el USB" },
		{ "Back up saves to USB", "Copiar saves para o USB", "Copiar partidas guardadas al USB" },
		{ "Run first-time setup again", "Refazer a configuração inicial", "Repetir la configuración inicial" },
		// controllers
		{ "Pair controllers (press A on each)", "Parear controles (aperte A em cada um)", "Emparejar mandos (pulsa A en cada uno)" },
		{ "Reset to Xbox layout", "Voltar ao layout do Xbox", "Restablecer el diseño de Xbox" },
		{ "Nintendo layout (A<->B, X<->Y)", "Layout da Nintendo (A<->B, X<->Y)", "Diseño de Nintendo (A<->B, X<->Y)" },
		{ "Press A on the controller for player %d", "Aperte A no controle do jogador %d", "Pulsa A en el mando del jugador %d" },
		// messages
		{ "OK", "OK", "Aceptar" },
		{ "Dismiss", "Fechar", "Cerrar" },
		{ "Hide", "Ocultar", "Ocultar" },
		{ "To report it, save the logs to your USB drive and attach them to a bug report on GitHub.",
			"Para relatar, salve os logs no seu USB e anexe-os a um relatório de bug no GitHub.",
			"Para informarlo, guarda los registros en tu USB y adjúntalos a un informe de error en GitHub." },
		// about, look
		{ "Cemu UWP by PrismFox67 (github.com/PrismFox67/Cemu-UWP)", "Cemu UWP por PrismFox67 (github.com/PrismFox67/Cemu-UWP)", "Cemu UWP por PrismFox67 (github.com/PrismFox67/Cemu-UWP)" },
		{ "Credits", "Créditos", "Créditos" },
		{ "Cemu and the Cemu logo by the Cemu team (cemu.info)", "Cemu e o logo do Cemu pela equipe do Cemu (cemu.info)", "Cemu y el logo de Cemu por el equipo de Cemu (cemu.info)" },
		{ "Title installer, texture allocation recovery and the flip pacing experiment adapted from danprice142/Cemu-UWP (MPL-2.0)",
			"Instalador de títulos, recuperação de alocação de texturas e o experimento de ritmo de quadros adaptados de danprice142/Cemu-UWP (MPL-2.0)",
			"Instalador de títulos, recuperación de asignación de texturas y el experimento de ritmo de fotogramas adaptados de danprice142/Cemu-UWP (MPL-2.0)" },
		{ "Box art from GameTDB (gametdb.com), downloaded for the games in your game folders",
			"Capas do GameTDB (gametdb.com), baixadas para os jogos das suas pastas de jogos",
			"Carátulas de GameTDB (gametdb.com), descargadas para los juegos de tus carpetas de juegos" },
		{ "Launcher design inspired by SternXD's Dolphin UWP", "Design do launcher inspirado no Dolphin UWP de SternXD", "Diseño del launcher inspirado en el Dolphin UWP de SternXD" },
		{ "Box art: download the games' covers from GameTDB", "Capas: baixar as capas dos jogos do GameTDB", "Carátulas: descargar las carátulas de los juegos de GameTDB" },
		{ "Download box art again", "Baixar as capas de novo", "Descargar las carátulas de nuevo" },
		{ "Menu sounds", "Sons do menu", "Sonidos del menú" },
		{ "Menu sounds: Kenney (kenney.nl, CC0)", "Sons do menu: Kenney (kenney.nl, CC0)", "Sonidos del menú: Kenney (kenney.nl, CC0)" },
		{ "Menu music", "Música do menu", "Música del menú" },
		{ "Look for the file again", "Procurar o arquivo de novo", "Buscar el archivo de nuevo" },
		{ "Cemu is not affiliated with Nintendo. Wii U is a trademark of Nintendo. Only play games you own.",
			"O Cemu não é afiliado à Nintendo. Wii U é uma marca registrada da Nintendo. Jogue apenas jogos que você possui.",
			"Cemu no está afiliado a Nintendo. Wii U es una marca registrada de Nintendo. Juega solo a juegos que tengas." },
	};

	const std::vector<HostTranslation::Language> kLanguages = {
		{ "", "Follow console language" },
		{ "en", "English" },
		{ "pt", "Português (Brasil)" },
		{ "es", "Español" },
	};

	std::mutex s_mutex;
	// one catalog per language, loaded on first use and kept, so texts returned before a language change stay valid
	std::unordered_map<std::string, std::unordered_map<std::string, std::string>> s_catalogs;

	// own texts of the user (or a tester translating): "English text<TAB>translation" per line, # starts a comment
	void _LoadOverlay(const std::string& code, std::unordered_map<std::string, std::string>& catalog)
	{
		std::ifstream file(ActiveSettings::GetUserDataPath("translations/{}.txt", code));
		std::string line;
		while (std::getline(file, line))
		{
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			const size_t tab = line.find('\t');
			if (line.empty() || line[0] == '#' || tab == std::string::npos || tab == 0 || tab + 1 == line.size())
				continue;
			catalog[line.substr(0, tab)] = line.substr(tab + 1);
		}
	}

	void _Load(const std::string& code, std::unordered_map<std::string, std::string>& catalog)
	{
		if (code == "en")
			return;
		for (const Entry& entry : kTable)
		{
			const char* text = code == "pt" ? entry.pt : code == "es" ? entry.es : nullptr;
			if (text)
				catalog.emplace(entry.english, text);
		}
		_LoadOverlay(code, catalog);
	}
}

const std::vector<HostTranslation::Language>& HostTranslation::GetLanguages()
{
	return kLanguages;
}

std::string HostTranslation::GetActiveLanguage()
{
	const std::string setting = GetConfig().host_ui_language.GetValue();
	if (!setting.empty())
		return setting;
	switch (GetConfig().console_language.GetValue())
	{
	case CafeConsoleLanguage::PT: return "pt";
	case CafeConsoleLanguage::ES: return "es";
	default: return "en";
	}
}

const char* HostTranslation::Tr(const char* english)
{
	const std::string code = GetActiveLanguage();
	std::unique_lock _l(s_mutex);
	auto catalogIt = s_catalogs.find(code);
	if (catalogIt == s_catalogs.end())
	{
		catalogIt = s_catalogs.emplace(code, std::unordered_map<std::string, std::string>()).first;
		_Load(code, catalogIt->second);
	}
	auto it = catalogIt->second.find(english);
	return it == catalogIt->second.end() ? english : it->second.c_str();
}
