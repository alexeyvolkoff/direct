/**
 * Shared Timeline, Scrubber, Back-Stack, and Smart Auto-Albums engine
 * powering Media Library, Creations Studio Gallery, and File Manager CameraRoll.
 */

import { hasSensitiveReadmeContent, hasSensitiveTag } from "./content-mode.js";
import { parseStoragePath, DEFAULT_STORAGE_PATH } from "./storage-path.js";
import { deviceRegistry } from "./device-registry.js";

const IMAGE_EXTS = new Set(["jpg", "jpeg", "png", "webp", "gif", "avif", "heic", "heif", "bmp", "tiff"]);
const VIDEO_EXTS = new Set(["mp4", "mov", "webm", "m4v", "mkv", "avi"]);

export const NOISE_MEDIA_TAGS = new Set([
  "photo", "photos", "image", "images", "picture", "video", "videos",
  "gps", "ocr", "person", "people", "portrait_raw", "portrait", "group", "omd",
  "sensitive", "nsfw", "document", "note", "file", "local", "mobile",
  "camera", "dcim", "img", "vid", "pxl", "ai_generated", "screenshot"
]);

// High-precision semantic canonicalization for mobile ML Kit, Apple Vision, and LLM tags.
// Keeps pets (cat, dog), wildlife, landmarks, specific events, distinctive nature, and sports.
export const PHOTO_TAG_CANONICAL_MAP = new Map([
  // Cats (котики)
  ["cat", "cat"], ["cats", "cat"], ["kitten", "cat"], ["kittens", "cat"], ["kitty", "cat"],
  ["felidae", "cat"], ["feline", "cat"], ["small to medium-sized cats", "cat"],
  ["domestic short-haired cat", "cat"], ["domestic long-haired cat", "cat"],
  ["tabby", "cat"], ["tabby cat", "cat"], ["persian cat", "cat"], ["siamese", "cat"],
  ["maine coon", "cat"], ["british shorthair", "cat"], ["scottish fold", "cat"],
  ["bengal", "cat"], ["sphynx", "cat"], ["ragdoll", "cat"],
  ["кот", "cat"], ["котик", "cat"], ["котики", "cat"], ["кошка", "cat"], ["кошки", "cat"],
  ["кіт", "cat"], ["кішка", "cat"], ["кошеня", "cat"],

  // Dogs (пёсики / собаки)
  ["dog", "dog"], ["dogs", "dog"], ["puppy", "dog"], ["puppies", "dog"],
  ["canidae", "dog"], ["canine", "dog"], ["companion dog", "dog"], ["working dog", "dog"],
  ["sporting group", "dog"], ["non-sporting group", "dog"], ["toy dog", "dog"], ["dog breed", "dog"],
  ["hound", "dog"], ["terrier", "dog"], ["retriever", "dog"], ["golden retriever", "dog"],
  ["labrador", "dog"], ["labrador retriever", "dog"], ["german shepherd", "dog"],
  ["french bulldog", "dog"], ["bulldog", "dog"], ["poodle", "dog"], ["beagle", "dog"],
  ["rottweiler", "dog"], ["husky", "dog"], ["siberian husky", "dog"], ["alaskan malamute", "dog"],
  ["corgi", "dog"], ["welsh corgi", "dog"], ["dachshund", "dog"], ["chihuahua", "dog"],
  ["pug", "dog"], ["pomeranian", "dog"], ["shiba inu", "dog"], ["border collie", "dog"],
  ["collie", "dog"], ["australian shepherd", "dog"], ["dobermann", "dog"], ["boxer", "dog"],
  ["great dane", "dog"], ["schnauzer", "dog"], ["spaniel", "dog"], ["cocker spaniel", "dog"],
  ["maltese", "dog"], ["yorkshire terrier", "dog"],
  ["собака", "dog"], ["собаки", "dog"], ["пес", "dog"], ["пёс", "dog"], ["щенок", "dog"], ["цуценя", "dog"],

  // Other distinct animals & wildlife
  ["bird", "bird"], ["birds", "bird"], ["parrot", "bird"], ["swan", "bird"], ["duck", "bird"],
  ["eagle", "bird"], ["owl", "bird"], ["seagull", "bird"], ["flamingo", "bird"], ["peacock", "bird"],
  ["horse", "horse"], ["horses", "horse"], ["stallion", "horse"], ["pony", "horse"], ["equestrian", "horse"],
  ["wildlife", "wildlife"], ["deer", "wildlife"], ["bear", "wildlife"], ["fox", "wildlife"],
  ["squirrel", "wildlife"], ["rabbit", "wildlife"], ["monkey", "wildlife"], ["elephant", "wildlife"],
  ["lion", "wildlife"], ["tiger", "wildlife"], ["giraffe", "wildlife"], ["zebra", "wildlife"],
  ["aquarium", "aquarium"], ["dolphin", "marine life"], ["whale", "marine life"],
  ["sea turtle", "marine life"], ["coral reef", "coral reef"], ["jellyfish", "marine life"],
  ["butterfly", "butterfly"],

  // Landmarks, historic architecture & monuments
  ["landmark", "landmark"], ["landmarks", "landmark"], ["historic site", "historic site"],
  ["unesco", "historic site"], ["archaeological site", "ruins"], ["ancient history", "ruins"],
  ["ruins", "ruins"], ["castle", "castle"], ["chateau", "castle"], ["château", "castle"],
  ["fortress", "castle"], ["citadel", "castle"], ["palace", "palace"], ["presidential palace", "palace"],
  ["cathedral", "cathedral"], ["basilica", "cathedral"], ["church", "church"], ["chapel", "church"],
  ["monastery", "monastery"], ["abbey", "monastery"], ["convent", "monastery"],
  ["temple", "temple"], ["pagoda", "temple"], ["shrine", "temple"], ["mosque", "mosque"], ["synagogue", "synagogue"],
  ["monument", "monument"], ["memorial", "monument"], ["obelisk", "monument"],
  ["triumphal arch", "monument"], ["statue", "statue"], ["sculpture", "sculpture"],
  ["fountain", "fountain"], ["tower", "tower"], ["clock tower", "tower"], ["bell tower", "tower"],
  ["lighthouse", "lighthouse"], ["windmill", "windmill"],
  ["bridge", "bridge"], ["arch bridge", "bridge"], ["suspension bridge", "bridge"], ["viaduct", "bridge"], ["aqueduct", "bridge"],
  ["museum", "museum"], ["art gallery", "museum"], ["exhibition", "exhibition"], ["art exhibition", "exhibition"],
  ["amphitheatre", "amphitheatre"], ["opera house", "opera"], ["stadium", "stadium"], ["arena", "stadium"],

  // Specific recognizable events & celebrations (never bare #event / #fun / #leisure)
  ["concert", "concert"], ["rock concert", "concert"], ["live music", "concert"], ["symphony", "concert"], ["orchestra", "concert"],
  ["music festival", "festival"], ["festival", "festival"], ["carnival", "carnival"], ["parade", "parade"],
  ["stage performance", "theater"], ["performing arts", "theater"], ["theater", "theater"], ["theatre", "theater"], ["opera", "opera"], ["ballet", "theater"],
  ["conference", "conference"], ["academic conference", "conference"], ["convention", "conference"], ["summit", "conference"],
  ["seminar", "conference"], ["lecture", "conference"], ["presentation", "conference"], ["public speaking", "conference"],
  ["wedding", "wedding"], ["marriage", "wedding"], ["bride", "wedding"], ["wedding ceremony", "wedding"], ["wedding reception", "wedding"],
  ["birthday", "birthday"], ["birthday cake", "birthday"], ["graduation", "graduation"],
  ["fireworks", "fireworks"], ["bonfire", "bonfire"], ["campfire", "camping"],
  ["christmas", "christmas"], ["christmas tree", "christmas"], ["christmas eve", "christmas"],
  ["new year", "new year"], ["new year's eve", "new year"], ["halloween", "halloween"],
  ["marathon", "marathon"],

  // Distinctive nature, landscapes & phenomena (never bare #sky / #cloud / #tree / #grass)
  ["mountain", "mountains"], ["mountains", "mountains"], ["mountain range", "mountains"], ["alps", "mountains"], ["mountainous landforms", "mountains"],
  ["peak", "mountains"], ["summit", "mountains"], ["glacier", "glacier"], ["volcano", "volcano"],
  ["canyon", "canyon"], ["gorge", "canyon"], ["cliff", "cliff"], ["cave", "cave"], ["cavern", "cave"],
  ["waterfall", "waterfall"], ["lake", "lake"], ["glacial lake", "lake"], ["lagoon", "lake"],
  ["fjord", "fjord"], ["river", "river"],
  ["beach", "beach"], ["coast", "beach"], ["seashore", "beach"], ["sea", "sea"], ["ocean", "sea"],
  ["island", "island"], ["archipelago", "island"], ["desert", "desert"], ["dune", "dunes"], ["sand dune", "dunes"],
  ["forest", "forest"], ["jungle", "forest"], ["rainforest", "forest"], ["woods", "forest"],
  ["autumn", "autumn"], ["fall foliage", "autumn"], ["deciduous", "autumn"],
  ["snow", "snow"], ["winter", "winter"], ["blizzard", "snow"], ["ice", "snow"],
  ["sunset", "sunset"], ["afterglow", "sunset"], ["dusk", "sunset"], ["sunrise", "sunrise"], ["dawn", "sunrise"],
  ["aurora", "aurora"], ["northern lights", "aurora"], ["rainbow", "rainbow"],

  // Sports, outdoor activities & travel modes (never generic #vehicle / #car)
  ["hiking", "hiking"], ["backpacking", "hiking"], ["trekking", "hiking"], ["mountaineering", "hiking"],
  ["skiing", "skiing"], ["alpine skiing", "skiing"], ["snowboarding", "snowboarding"],
  ["cycling", "cycling"], ["bicycling", "cycling"], ["mountain biking", "cycling"],
  ["surfing", "surfing"], ["sailing", "sailing"], ["sailboat", "sailing"], ["yacht", "yacht"], ["boat", "boat"], ["ship", "ship"],
  ["kayaking", "kayaking"], ["canoeing", "kayaking"], ["rafting", "kayaking"],
  ["swimming", "swimming"], ["diving", "diving"], ["underwater", "diving"], ["snorkeling", "diving"],
  ["camping", "camping"], ["tent", "camping"],
  ["airplane", "aviation"], ["aircraft", "aviation"], ["airliner", "aviation"], ["aviation", "aviation"], ["flight", "aviation"],
  ["train", "train"], ["railway", "train"], ["locomotive", "train"],

  // Specific gastronomy (requires higher confidence on ML Kit)
  ["sushi", "sushi"], ["pizza", "pizza"], ["seafood", "seafood"], ["barbecue", "barbecue"], ["grill", "barbecue"],
  ["cake", "dessert"], ["dessert", "dessert"], ["pastry", "dessert"], ["ice cream", "dessert"], ["gelato", "dessert"],
  ["wine", "wine"], ["winery", "wine"], ["vineyard", "wine"],
]);

// Exhaustive trash filter: clothing, furniture, sky/building ubiquity, car parts, human/animal anatomy, abstract filler
export const PHOTO_TRASH_TAGS = new Set([
  // Clothing, fashion, footwear, eyewear & accessories
  "jeans", "denim", "jacket", "coat", "blazer", "trench coat", "parka", "windbreaker", "overcoat",
  "suit", "tuxedo", "formal wear", "shirt", "t-shirt", "tee", "polo shirt", "dress shirt", "blouse",
  "top", "crop top", "tank top", "sleeveless shirt", "sweater", "pullover", "jumper", "cardigan",
  "hoodie", "sweatshirt", "fleece", "vest", "waistcoat", "trousers", "pants", "chinos", "khakis",
  "sweatpants", "shorts", "trunks", "leggings", "tights", "skirt", "miniskirt", "dress", "gown",
  "cocktail dress", "sundress", "day dress", "robe", "pajamas", "lingerie", "undergarment", "swimwear", "bikini", "brassiere",
  "sleeve", "long-sleeved t-shirt", "collar", "lapel", "pocket", "button", "zipper", "belt", "buckle",
  "tie", "necktie", "bow tie", "scarf", "shawl", "glove", "mitten", "sock", "stocking",
  "shoe", "shoes", "footwear", "sneakers", "trainer", "athletic shoe", "walking shoe", "outdoor shoe",
  "boot", "boots", "sandal", "slipper", "flip-flops", "high heels", "high-heeled footwear", "loafer", "oxford shoe",
  "hat", "cap", "baseball cap", "beanie", "knit cap", "sun hat", "fedora", "beret", "cowboy hat", "helmet", "headgear", "headpiece",
  "glasses", "eyeglasses", "eyewear", "sunglasses", "goggles", "vision care",
  "watch", "wristwatch", "clock", "jewelry", "jewellery", "body jewelry", "necklace", "pendant", "locket",
  "bracelet", "bangle", "earring", "ring", "wedding ring", "brooch",
  "bag", "handbag", "purse", "tote bag", "shoulder bag", "messenger bag", "clutch", "backpack",
  "luggage", "luggage and bags", "suitcase", "baggage", "briefcase", "wallet", "umbrella",
  "clothing", "clothes", "apparel", "garment", "attire", "outfit", "outerwear", "sportswear",
  "activewear", "streetwear", "workwear", "uniform", "jersey", "costume",
  "fashion", "fashion design", "fashion accessory", "fashion model", "street fashion",
  "textile", "fabric", "cloth", "woven fabric", "cotton", "linen", "silk", "wool", "woolen", "cashmere",
  "leather", "leather jacket", "suede", "fur", "fur clothing", "velvet", "satin", "lace", "mesh", "nylon", "polyester",
  "knitwear", "knitting", "plaid", "tartan", "stripe", "polka dot", "check", "pattern", "motif",
  "джинсы", "джинси", "денон", "деним", "денім", "пиджак", "піджак", "жакет", "куртка", "пальто",
  "рубашка", "сорочка", "футболка", "кофта", "свитер", "светр", "брюки", "штани", "шорты", "шорти",
  "платье", "сукня", "юбка", "спідниця", "костюм", "одежда", "одяг", "обувь", "взуття", "кроссовки", "кросівки",
  "туфли", "туфлі", "ботинки", "черевики", "очки", "окуляри", "шапка", "шляпа", "кепка", "сумка", "рюкзак",

  // Furniture, room fixtures, tableware & household objects
  "chair", "chairs", "armchair", "office chair", "club chair", "folding chair", "rocking chair", "wheelchair",
  "stool", "bar stool", "footstool", "bench", "outdoor bench", "pew",
  "table", "tables", "desk", "computer desk", "writing desk", "coffee table", "dining table", "outdoor table", "picnic table", "side table", "nightstand",
  "couch", "sofa", "loveseat", "studio couch", "sofa bed", "futon", "chaise longue",
  "bed", "bed frame", "bunk bed", "infant bed", "mattress", "pillow", "cushion", "throw pillow", "blanket", "duvet", "comforter", "bed sheet", "bedding", "linens",
  "cabinet", "cabinetry", "cupboard", "wardrobe", "closet", "drawer", "chest of drawers", "dresser", "filing cabinet", "sideboard",
  "shelf", "shelving", "bookcase", "bookshelf", "rack", "coat rack", "countertop", "worktop", "kitchen island",
  "curtain", "drapery", "window blind", "window shade", "window treatment", "window covering", "valance", "shutter",
  "carpet", "rug", "mat", "doormat", "floor", "flooring", "wood flooring", "hardwood", "laminate flooring", "tile", "tile flooring", "linoleum", "parquet",
  "ceiling", "dropped ceiling", "wall", "drywall", "plaster", "wallpaper", "molding", "baseboard", "wainscoting", "paneling",
  "door", "screen door", "sliding door", "glass door", "doorknob", "door handle", "hinge", "lock",
  "window", "sash window", "casement window", "bay window", "skylight", "windowsill", "mirror",
  "stairs", "staircase", "step", "handrail", "railing", "baluster", "banister", "escalator", "elevator",
  "balcony", "porch", "patio", "deck", "veranda", "terrace", "pergola", "awning", "canopy", "gazebo",
  "hallway", "corridor", "foyer", "lobby", "room", "living room", "family room", "bedroom", "bathroom", "restroom",
  "kitchen", "dining room", "laundry room", "basement", "attic", "closet", "office", "classroom", "conference hall", "auditorium", "function hall", "banquet hall",
  "interior", "interior design", "furniture", "street furniture", "outdoor furniture",
  "home", "house", "cottage", "townhouse", "apartment", "condominium", "real estate", "property", "estate",
  "appliance", "home appliance", "kitchen appliance", "major appliance", "small appliance",
  "refrigerator", "freezer", "microwave", "microwave oven", "oven", "stove", "kitchen stove", "cooktop", "range hood",
  "dishwasher", "washing machine", "clothes dryer", "vacuum cleaner", "iron", "air conditioner", "heater", "radiator", "fan", "ceiling fan",
  "sink", "kitchen sink", "bathroom sink", "faucet", "tap", "plumbing", "plumbing fixture", "toilet", "bidet", "bathtub", "shower",
  "lamp", "lampshade", "light fixture", "lighting", "chandelier", "sconce", "pendant light", "track lighting", "fluorescent lamp", "incandescent light bulb", "led lamp", "lantern", "candle", "daylighting",
  "plate", "platter", "dish", "dishware", "tableware", "serveware", "porcelain", "ceramic", "earthenware", "pottery", "stoneware",
  "bowl", "mixing bowl", "cup", "mug", "teacup", "coffee cup", "saucer", "glassware", "stemware", "barware",
  "wine glass", "champagne stemware", "beer glass", "pint glass", "highball glass", "old fashioned glass", "shot glass", "tumbler", "drinking glass",
  "bottle", "water bottle", "plastic bottle", "glass bottle", "wine bottle", "beer bottle", "can", "tin can", "aluminum can", "beverage can",
  "jar", "mason jar", "jug", "pitcher", "carafe", "kettle", "teapot", "coffeemaker", "espresso machine",
  "cutlery", "flatware", "kitchen utensil", "cookware and bakeware", "pan", "frying pan", "pot", "wok", "fork", "spoon", "knife", "kitchen knife", "table knife", "chopsticks", "spatula", "ladle", "whisk", "tongs",
  "straw", "drinking straw", "napkin", "paper towel", "tissue", "tablecloth", "placemat", "coaster", "tray", "serving tray", "cutting board",
  "trash can", "waste container", "recycle bin", "bucket", "basket", "box", "shipping box", "carton", "cardboard", "packaging and labeling", "plastic bag",
  "paper", "paper product", "stationery", "notebook", "binder", "envelope", "pen", "ballpoint pen", "pencil", "marker", "scissors", "tape",
  "cord", "cable", "wire", "electrical wiring", "electrical cable", "power cord", "extension cord", "plug", "socket", "power strip", "switch",
  "стул", "стілець", "стулья", "стільці", "стол", "стіл", "кресло", "крісло", "диван", "кровать", "ліжко",
  "шкаф", "шафа", "полка", "полиця", "мебель", "меблі", "комната", "кімната", "интерьер", "інтер'єр",
  "окно", "вікно", "дверь", "двері", "стена", "стіна", "пол", "підлога", "потолок", "стеля", "лампа",
  "тарелка", "тарілка", "чашка", "кружка", "стакан", "бокал", "бутылка", "пляшка", "посуда", "посуд", "вилка", "ложка", "нож", "ніж",

  // Ubiquitous outdoor: sky, clouds, generic buildings, streets, materials & generic plants
  "sky", "cloud", "clouds", "cumulus", "cirrus", "stratus", "nimbus", "daytime", "night", "evening", "morning",
  "atmosphere", "air", "weather", "meteorological phenomenon", "atmospheric phenomenon",
  "light", "sunlight", "sunbeam", "ray", "flare", "lens flare", "shadow", "shade", "tint", "reflection", "glare", "horizon",
  "building", "buildings", "structure", "infrastructure", "nonbuilding structure", "architecture", "classical architecture", "medieval architecture", "modern architecture", "brutalist architecture",
  "urban area", "urban design", "metropolitan area", "metropolis", "city", "cityscape", "skyline", "downtown", "town", "town square", "village", "suburb", "neighbourhood", "neighborhood", "human settlement",
  "residential area", "commercial building", "mixed-use", "office building", "headquarters", "corporate headquarters", "skyscraper", "tower block", "high-rise building", "industrial", "factory", "warehouse", "shed", "barn", "garage",
  "facade", "roof", "rooftop", "shingle", "siding", "eaves", "gutter", "chimney", "gable", "arch", "arcade", "column", "pillar", "beam", "truss", "girder", "scaffolding",
  "brick", "brickwork", "masonry", "stone wall", "cobblestone", "flagstone", "concrete", "reinforced concrete", "cement", "mortar", "stucco", "asphalt", "tar", "bitumen",
  "road", "road surface", "thoroughfare", "highway", "controlled-access highway", "freeway", "expressway", "street", "alley", "lane", "avenue", "boulevard", "intersection", "roundabout", "overpass", "tunnel",
  "sidewalk", "pavement", "walkway", "footpath", "path", "trail", "boardwalk", "promenade", "crosswalk", "pedestrian crossing", "curb", "driveway", "parking", "parking lot", "parking space",
  "street light", "lamp post", "traffic light", "signaling device", "sign", "signage", "street sign", "traffic sign", "neon sign", "electronic signage", "billboard", "banner", "flag", "pole", "flagpole", "utility pole", "transmission tower", "overhead power line", "electricity", "public utility",
  "fence", "fencing", "wire fencing", "chain-link fencing", "split-rail fence", "picket fence", "gate", "guard rail", "barrier", "bollard", "bench", "hydrant", "manhole",
  "metal", "steel", "stainless steel", "iron", "wrought iron", "cast iron", "aluminum", "brass", "bronze", "copper", "tin", "chrome",
  "wood", "lumber", "timber", "plank", "plywood", "wood stain", "varnish", "particle board", "bamboo",
  "glass", "stained glass", "fiberglass", "plastic", "acrylic", "rubber", "silicone", "foam", "sponge",
  "stone", "rock", "bedrock", "outcrop", "boulder", "pebble", "gravel", "rubble", "sand", "soil", "dirt", "mud", "clay", "dust", "ground",
  "grass", "grass family", "lawn", "turf", "grassland", "pasture", "meadow", "prairie", "steppe", "field", "lot", "land lot", "yard", "backyard",
  "plant", "plants", "plant stem", "plant community", "terrestrial plant", "vascular plant", "non-vascular land plant", "flowering plant", "annual plant", "perennial plant",
  "woody plant", "herbaceous plant", "subshrub", "shrub", "bush", "hedge", "groundcover", "weed", "moss", "algae", "fern", "vine", "ivy",
  "houseplant", "flowerpot", "planter", "vase", "cut flowers", "bouquet", "flower", "petal", "blossom", "pollen",
  "tree", "trees", "shade tree", "fruit tree", "conifer", "evergreen", "pine", "pine family", "fir", "spruce", "oak", "birch", "palm tree", "arecales",
  "trunk", "bark", "branch", "twig", "root", "leaf", "leaves", "foliage", "canopy", "needle", "vegetation", "greenery", "flora", "botany",
  "nature", "natural", "natural environment", "natural landscape", "natural material", "landscape", "scenery", "ecoregion", "ecosystem", "biome", "habitat", "wilderness", "nature reserve", "park", "garden", "yard",
  "water", "water resources", "body of water", "watercourse", "waterway", "bank", "riparian zone", "wetland", "marsh", "pond", "puddle", "reflection pool", "liquid", "fluid", "wave", "wind wave", "foam",
  "world", "earth", "globe", "geography", "geology", "space", "outer space",
  "небо", "облака", "облако", "хмара", "хмари", "здание", "здания", "будівля", "будівлі", "дом", "будинок",
  "инфраструктура", "інфраструктура", "архитектура", "архітектура", "улица", "вулиця", "дорога", "тротуар", "асфальт",
  "дерево", "деревья", "дерева", "трава", "газон", "растение", "растения", "рослина", "рослини", "листья", "листя", "куст", "кущ", "природа", "пейзаж", "вода",

  // Generic vehicles & automotive parts
  "vehicle", "vehicles", "land vehicle", "motor vehicle", "commercial vehicle", "luxury vehicle", "family car", "compact car", "subcompact car", "mid-size car", "full-size car", "city car", "sports car", "supercar",
  "car", "cars", "automobile", "sedan", "hatchback", "station wagon", "coupe", "convertible", "suv", "sport utility vehicle", "crossover", "minivan", "van", "panel van", "campervan", "truck", "pickup truck", "semi-trailer truck", "bus", "tour bus", "public transport", "taxi",
  "motorcycle", "scooter", "moped", "bicycle", "bicycle wheel", "bicycle frame", "bicycle handlebar",
  "automotive design", "automotive exterior", "automotive lighting", "automotive tire", "automotive wheel system", "automotive parking light", "automotive tail & brake light", "automotive mirror", "automotive side-view mirror", "automotive windowPart",
  "headlamp", "fogucha", "bumper", "grille", "hood", "bonnet", "fender", "quarter panel", "spoiler", "exhaust", "muffler", "windshield", "windscreen", "wiper", "car door", "vehicle door", "door handle", "rear-view mirror", "dashboard", "steering wheel", "car seat", "seat belt",
  "auto part", "engine", "machine", "wheel", "rim", "alloy wheel", "hubcap", "spoke", "tire", "tread", "synthetic rubber", "tire care",
  "license plate", "vehicle registration plate", "traffic", "traffic congestion", "transport", "transportation", "mode of transport", "rolling",
  "машина", "автомобиль", "автомобіль", "авто", "транспорт", "колесо",

  // Human & animal anatomy, poses, emotions, colors, electronics & abstract ML Kit filler
  "person", "people", "human", "human body", "human anatomy", "man", "men", "woman", "women", "gentleman", "lady", "boy", "girl", "adult", "youth", "teenager", "child", "kid", "toddler", "infant", "baby",
  "face", "facial expression", "head", "skull", "forehead", "temple", "eyebrow", "eyelash", "eyelid", "eye", "iris", "pupil", "nose", "nostril", "cheek", "ear", "mouth", "lip", "lipstick", "tooth", "teeth", "tongue", "chin", "double chin", "jaw",
  "neck", "throat", "collarbone", "shoulder", "arm", "upper arm", "elbow", "forearm", "wrist", "hand", "palm", "finger", "thumb", "index finger", "ring finger", "nail", "manicure",
  "chest", "thorax", "torso", "trunk", "abdomen", "stomach", "navel", "waist", "back", "spine", "hip", "buttocks", "lap", "leg", "human leg", "thigh", "knee", "calf", "shin", "ankle", "foot", "barefoot", "heel", "toe",
  "skin", "flesh", "epidermis", "wrinkle", "freckle", "mole", "tattoo", "muscle", "joint", "limb", "organ", "organism",
  "hair", "hairstyle", "hair coloring", "hair care", "feathered hair", "step cutting", "layered hair", "bob cut", "pixie cut", "bangs", "fringe", "ponytail", "pigtail", "braid", "bun", "afro", "dreadlocks", "wig",
  "blond", "blonde", "brown hair", "black hair", "red hair", "grey hair", "long hair", "short hair", "curly hair", "straight hair",
  "facial hair", "beard", "moustache", "mustache", "goatee", "sideburns", "stubble", "eyelash extensions", "cosmetics", "makeover",
  "smile", "smiling", "laugh", "laughter", "grin", "smirk", "frown", "pout", "wink", "gaze",
  "happy", "happiness", "joy", "pleasure", "excitement", "enthusiasm", "fun", "leisure", "recreation", "relaxation", "comfort", "calm", "cool", "beauty", "youth", "cute",
  "pose", "posture", "balance", "gesture", "pointing", "thumbs signal", "wave", "standing", "sitting", "kneeling", "lying down", "leaning", "walking", "strolling", "running", "jogging", "jumping",
  "talking", "speech", "conversation", "discussion", "meeting", "interaction", "communication", "sharing", "cooperation", "collaboration", "job", "employment", "white-collar worker", "businessperson", "official", "spokesperson",
  "friendship", "family", "parent", "couple", "romance", "love", "affection", "hug", "kiss", "holding hands",
  "crowd", "audience", "spectator", "fan", "social group", "community", "team", "club", "organization", "youth",
  "event", "events", "public event", "ceremony", "tradition", "ritual", "entertainment", "nightlife",
  "selfie", "snapshot", "photography", "photograph", "photographer", "photo", "picture", "image", "stock photography", "photo shoot", "flash photography", "backlighting", "long exposure", "bokeh", "depth of field",
  "portrait", "portrait photography", "headshot", "close-up", "macro photography", "still life", "still life photography", "monochrome", "monochrome photography", "black-and-white", "sepia", "vintage",
  "art", "visual arts", "modern art", "contemporary art", "painting", "drawing", "sketch", "illustration", "graphic design", "graphics", "clip art", "collage",
  "color", "colour", "colorfulness", "tint", "hue", "pastel", "neon", "magenta", "purple", "violet", "lavender", "lilac", "indigo", "blue", "electric blue", "cobalt blue", "navy blue", "royal blue", "sky blue", "azure", "cyan", "aqua", "turquoise", "teal",
  "green", "lime", "olive", "emerald", "yellow", "amber", "gold", "mustard", "orange", "coral", "peach", "apricot", "red", "crimson", "scarlet", "burgundy", "maroon", "carmine", "pink", "rose", "salmon",
  "brown", "tan", "beige", "cream", "ivory", "khaki", "ochre", "copper", "bronze", "silver", "grey", "gray", "charcoal", "black", "white", "darkness",
  "font", "typography", "text", "letter", "character", "number", "numeral", "symbol", "icon", "emblem", "crest", "coat of arms", "logo", "trademark", "brand", "label", "sticker", "badge",
  "advertising", "advertisement", "marketing", "publication", "magazine", "newspaper", "book", "page", "book cover", "comic", "poster", "flyer", "brochure", "menu", "receipt", "invoice", "ticket", "card", "business card", "greeting card", "id card", "document", "handwriting", "calligraphy", "signature",
  "line", "curve", "pattern", "mesh", "grid", "net", "checkerboard", "rectangle", "square", "circle", "sphere", "oval", "ellipse", "triangle", "polygon", "cylinder", "cone", "spiral", "helix", "symmetry", "parallel", "perpendicular", "slope", "angle", "point", "surface", "texture",
  "gadget", "electronic device", "electronics", "consumer electronics", "electronic component", "circuit", "technology", "engineering",
  "display device", "flat panel display", "liquid-crystal display", "led display", "oled", "plasma display", "projection screen", "projector", "screen", "touchscreen",
  "computer monitor", "television", " television set", "smart tv", "laptop", "netbook", "ultrabook", "personal computer", "desktop computer", "computer", "computer hardware", "computer component", "computer keyboard", "space bar", "mouse", "touchpad", "tablet", "tablet computer",
  "mobile phone", "smartphone", "cell phone", "feature phone", "iphone", "telephone", "telephony", "portable communications device", "communication device", "mobile device", "case", "phone case",
  "audio equipment", "microphone", "loudspeaker", "speaker", "headphones", "headset", "earbuds", "camera", "digital camera", "slr camera", "mirrorless camera", "video camera", "webcam", "camera lens", "tripod", "flash",
  "multimedia", "media", "software", "application", "mobile app", "user interface", "website", "web page", "world wide web", "internet", "screenshot", "operating system",
  "food", "ingredient", "recipe", "dish", "meal", "breakfast", "brunch", "lunch", "dinner", "supper", "snack", "finger food", "fast food", "junk food", "comfort food", "street food", "staple food", "natural foods", "whole food", "superfood", "local food", "vegan nutrition", "vegetarian food", "produce", "vegetable", "leaf vegetable", "root vegetable", "fruit", "berry", "citrus", "bread", "baked goods", "baking", "gluten", "dairy", "meat", "poultry", "cooking", "culinary arts", "cuisine", "gastronomy", "drink", "beverage", "alcoholic beverage", "distilled beverage", "liqueur", "beer", "cocktail", "soft drink", "juice", "tea", "coffee",
  // Animal anatomy noise emitted alongside Cat / Dog by ML Kit
  "animal", "animals", "terrestrial animal", "working animal", "pack animal", "domestic animal", "pet", "pets",
  "mammal", "vertebrate", "invertebrate", "carnivore", "carnivora", "herbivore", "omnivore",
  "whiskers", "snout", "muzzle", "nose", "fur", "coat", "paw", "claw", "hoof", "horn", "antler", "beak", "feather", "wing", "fin", "scale", "tail", "collar", "dog collar", "leash", "kennel", "cage",
  "человек", "людина", "люди", "мужчина", "чоловік", "женщина", "жінка", "лицо", "обличчя", "улыбка", "посмішка",
  "рука", "волосы", "волосся", "толпа", "натовп", "событие", "подія", "мероприятие", "захід", "отдых", "відпочинок", "досуг", "веселье", "розваги", "селфи", "селфі", "портрет"
]);

// Substring guard for compound clothing/furniture/automotive/material ML Kit labels
const PHOTO_TRASH_SUBSTRING_RE = /\b(shirt|blouse|jeans|denim|jacket|blazer|coat|sweater|hoodie|cardigan|trousers|pants|shorts|skirt|leggings|footwear|sneaker|sandal|slipper|heels|eyewear|glasses|sunglasses|handbag|backpack|luggage|underwear|lingerie|swimwear|clothing|apparel|outerwear|sportswear|fashion|textile|fabric|chair|stool|couch|sofa|mattress|pillow|cushion|blanket|bedding|cabinet|drawer|wardrobe|closet|bookshelf|bookcase|countertop|curtain|blind|flooring|carpet|ceiling|drywall|wallpaper|doorknob|staircase|handrail|furniture|appliance|refrigerator|microwave|dishwasher|faucet|toilet|bathtub|lampshade|light fixture|chandelier|tableware|dishware|serveware|glassware|stemware|cutlery|flatware|bottle|napkin|tablecloth|cardboard|packaging|automotive|motor vehicle|land vehicle|road surface|asphalt|sidewalk|pavement|infrastructure|residential area|urban area|metropolitan area|commercial building|office building|tower block|power line|utility pole|wire fencing|terrestrial plant|vascular plant|woody plant|herbaceous plant|houseplant|flowerpot|groundcover|facial hair|hairstyle|display device|computer|smartphone|mobile phone|telephony|whiskers|carnivore|vertebrate)\b/i;

// Canonical geographic aliases so cities/oblasts/countries belong ONLY in "Places", never duplicated as #hashtags
const GEO_CITY_CANONICAL_GROUPS = [
  ["uzhhorod", "užgorod", "uzgorod", "ужгород"],
  ["zakarpattia", "transcarpathia", "закарпаття", "закарпатська область", "закарпатская область"],
  ["lviv", "львів", "львов", "lviv oblast", "львівська область"],
  ["kyiv", "kiev", "київ", "киев", "kyiv oblast", "київська область"],
  ["odesa", "odessa", "одеса", "одесса"],
  ["kharkiv", "харків", "харьков"],
  ["ternopil", "тернопіль"],
  ["ivano-frankivsk", "івано-франківськ"],
  ["khmelnytskyi", "хмельницький"],
  ["vinnytsia", "вінниця"],
  ["mukachevo", "mukacheve", "мукачево", "мукачеве"],
  ["berehove", "берегове"],
  ["khust", "хуст"],
  ["chop", "чоп"],
  ["kranj", "крань"],
  ["ljubljana", "любляна"],
  ["maribor", "марибор"],
  ["celje", "целє", "целе"],
  ["koper", "копер"],
  ["bled", "блед"],
  ["budapest", "будапешт"],
  ["debrecen", "дебрецен"],
  ["vienna", "wien", "відень", "вена"],
  ["graz", "грац"],
  ["salzburg", "зальцбург"],
  ["bratislava", "братислава"],
  ["kosice", "košice", "кошіце", "кошиці"],
  ["presov", "prešov", "пряшів", "прешов"],
  ["prague", "praha", "прага"],
  ["warsaw", "warszawa", "варшава"],
  ["krakow", "kraków", "краків", "краков"],
  ["zagreb", "загреб"],
  ["split", "спліт"],
  ["dubrovnik", "дубровник"],
  ["london", "лондон"],
  ["paris", "париж"],
  ["rome", "roma", "рим"],
  ["barcelona", "барселона"],
  ["madrid", "мадрид"],
  ["berlin", "берлін", "берлин"],
  ["munich", "münchen", "мюнхен"],
];

const GEO_COUNTRIES_AND_REGIONS_SET = new Set([
  "ukraine", "україна", "украина", "slovenia", "slovenija", "словенія", "словения",
  "hungary", "magyarország", "угорщина", "венгрия", "slovakia", "slovensko", "словаччина", "словакия",
  "poland", "polska", "польща", "польша", "czech republic", "czechia", "česko", "чехія", "чехия",
  "austria", "österreich", "австрія", "австрия", "germany", "deutschland", "німеччина", "германия",
  "italy", "italia", "італія", "италия", "spain", "españa", "іспанія", "испания",
  "france", "франція", "франция", "united kingdom", "uk", "england", "great britain", "велика британія", "великобритания",
  "croatia", "hrvatska", "хорватія", "хорватия", "montenegro", "црна гора", "чорногорія", "черногория",
  "serbia", "србија", "сербія", "сербия", "romania", "românia", "румунія", "румыния",
  "bulgaria", "българия", "болгарія", "болгария", "greece", "ελλάδα", "греція", "греция",
  "turkey", "türkiye", "туреччина", "турция", "switzerland", "schweiz", "suisse", "швейцарія", "швейцария",
  "netherlands", "nederland", "нідерланди", "нидерланды", "belgium", "belgië", "бельгія", "бельгия",
  "portugal", "португалія", "португалия", "united states", "usa", "сша", "canada", "канада",
  "gorenjska", "upper carniola", "osrednjeslovenska", "gran canaria", "canary islands",
]);

const _CITY_TO_CANONICAL = new Map();
for (const group of GEO_CITY_CANONICAL_GROUPS) {
  const canonical = group[0];
  for (const alias of group) {
    _CITY_TO_CANONICAL.set(alias.toLowerCase(), canonical);
  }
}

export function canonicalizePlaceKey(rawPlace = "") {
  const low = String(rawPlace || "").trim().toLowerCase();
  if (!low) return "";
  return _CITY_TO_CANONICAL.get(low) || low;
}

export function isGeoOrRegionTag(rawTag = "", extraPlaceKeys = null) {
  const low = String(rawTag || "").trim().toLowerCase().replace(/^#+/, "");
  if (!low) return false;
  if (GEO_COUNTRIES_AND_REGIONS_SET.has(low)) return true;
  if (_CITY_TO_CANONICAL.has(low)) return true;
  // Administrative region suffixes in English, Ukrainian, Russian, Slovak, Slovenian
  if (/\b(oblast|область|обл|raion|район|край|region|регіон|регион|province|municipality|county|district|gmina|okres|županija|волость)\b/i.test(low)) {
    return true;
  }
  if (extraPlaceKeys && extraPlaceKeys.size > 0) {
    const canon = canonicalizePlaceKey(low);
    if (extraPlaceKeys.has(low) || extraPlaceKeys.has(canon)) return true;
  }
  return false;
}

/**
 * Filters out clothing, furniture, sky/building noise, anatomy, and duplicate geographic tags,
 * while canonicalizing cats, dogs, wildlife, landmarks, specific events, and nature themes.
 */
export function filterAndCanonicalizePhotoTags(
  rawTags = [],
  {
    extraPlaceKeys = null,
    peopleLower = null,
    deviceNamesLower = null,
    docFirstSeg = "",
    landmarkLower = "",
    eventLower = "",
    isCreation = false,
  } = {}
) {
  const out = [];
  const seen = new Set();

  for (const entry of rawTags || []) {
    const rawStr = typeof entry === "object" && entry !== null ? entry.text || entry.tag || "" : entry;
    const conf = typeof entry === "object" && entry !== null && typeof entry.confidence === "number"
      ? entry.confidence
      : 1.0;

    const clean = String(rawStr || "")
      .trim()
      .replace(/^#+/, "")
      .replace(/\s+/g, " ");
    let low = clean.toLowerCase();
    if (!low || low.length < 2 || low.length > 36) continue;
    if (/^\d+$/.test(low)) continue;
    if (NOISE_MEDIA_TAGS.has(low)) continue;
    if (docFirstSeg && low === docFirstSeg) continue;
    if (deviceNamesLower && deviceNamesLower.has(low)) continue;
    if (peopleLower && peopleLower.has(low)) continue;

    // Preserve explicit landmark or named event tags verbatim
    if ((landmarkLower && low === landmarkLower) || (eventLower && low === eventLower)) {
      if (!seen.has(low)) {
        seen.add(low);
        out.push(low);
      }
      continue;
    }

    // Never allow Creations style tags to bleed onto real camera photos
    if (!isCreation && (low === "cinematic & film" || low === "cafe & bistro" || low === "street & city" || low === "golden hour" || low === "nature & outdoors")) {
      continue;
    }

    // Canonicalize known meaningful tags first (e.g. "small to medium-sized cats" -> "cat", "companion dog" -> "dog")
    if (PHOTO_TAG_CANONICAL_MAP.has(low)) {
      const mapped = PHOTO_TAG_CANONICAL_MAP.get(low);
      // Require higher confidence for food/drink labels on ML Kit so a stray glass on a table doesn't tag the photo
      if (["sushi", "pizza", "seafood", "barbecue", "dessert", "wine"].includes(mapped) && conf < 0.72) {
        continue;
      }
      low = mapped;
    } else {
      // Drop geographic region/city/country duplicates (handled cleanly by Places albums)
      if (!isCreation && isGeoOrRegionTag(low, extraPlaceKeys)) continue;
      // Drop trash tags and compound clothing/furniture/automotive/sky/building phrases
      if (PHOTO_TRASH_TAGS.has(low)) continue;
      if (!isCreation && PHOTO_TRASH_SUBSTRING_RE.test(low)) continue;
    }

    if (!isCreation && isGeoOrRegionTag(low, extraPlaceKeys)) continue;
    if (PHOTO_TRASH_TAGS.has(low)) continue;

    if (!seen.has(low)) {
      seen.add(low);
      out.push(low);
    }
  }

  return out;
}

const LANDMARK_TITLE_RE = /\b(Castle|Citadel|Fortress|Palace|Château|Chateau|Cathedral|Basilica|Monastery|Abbey|Church\s+of|Temple\s+of|Mosque|Synagogue|Museum|National\s+Gallery|Monument|Memorial|Obelisk|Triumphal\s+Arch|Clock\s+Tower|Bell\s+Tower|Big\s+Ben|Eiffel\s+Tower|Tower\s+Bridge|Charles\s+Bridge|Chain\s+Bridge|Triple\s+Bridge|Dragon\s+Bridge|Colosseum|Acropolis|Pantheon|Parthenon|Sagrada\s+Fam[ií]lia|Alhambra|Louvre|Hermitage|Kremlin|Capitol|Westminster|Sand\s+Dunes|National\s+Park|Waterfall|Falls|Canyon|Glacier|Lake\s+[A-ZА-ЯІЇЄ]|Mount\s+[A-ZА-ЯІЇЄ]|Замок|Фортеця|Палац|Собор|Монастир|Музей|Водоспад|Озеро\s+[А-ЯІЇЄ]|Grad\s+[A-ZČŠŽ])\b/i;

const GENERIC_NON_LANDMARK_TITLE_RE = /\b(portrait|selfie|group\s+photo|dinner|lunch|breakfast|cat|dog|screen|slide|document|invoice|receipt|man\s+in|woman\s+in|two\s+people|street\s+scene|parking|car\b)/i;

/**
 * Extracts explicit Landmark / Event metadata from card text or Gemma's '# Title' header.
 */
export function extractCardLandmarkAndEvent(rawText = "", rawTitle = "", city = "") {
  const txt = String(rawText || "");
  let landmark = "";
  let event = "";

  const mLandmark = txt.match(/(?:^|\n)Landmark:\s*([^\n]+)/i);
  if (mLandmark) {
    landmark = mLandmark[1].trim().replace(/\s*\([^)]*\)\s*$/, "").replace(/\.$/, "").trim();
  }

  const mEvent = txt.match(/(?:^|\n)Event:\s*([^\n]+)/i);
  if (mEvent) {
    event = mEvent[1].trim().replace(/\.$/, "").trim();
  }

  // Check markdown '# Title' from Gemma sidecars if no explicit Landmark line exists
  if (!landmark) {
    const mMdTitle = txt.match(/^#\s+([^\n]+)/);
    const candidateTitle = (mMdTitle ? mMdTitle[1] : rawTitle || "").trim();
    if (
      candidateTitle &&
      candidateTitle.length >= 4 &&
      candidateTitle.length <= 64 &&
      !/\.(jpe?g|png|webp|mp4|mov)$/i.test(candidateTitle) &&
      !GENERIC_NON_LANDMARK_TITLE_RE.test(candidateTitle) &&
      LANDMARK_TITLE_RE.test(candidateTitle)
    ) {
      // Strip trailing ", City" or ", Country" so "Uzhhorod Castle, Ukraine" -> "Uzhhorod Castle"
      let cleaned = candidateTitle.split(/\s*[\u2014\u2013|]\s*/)[0].trim();
      const commaParts = cleaned.split(",").map((s) => s.trim()).filter(Boolean);
      if (commaParts.length > 1 && LANDMARK_TITLE_RE.test(commaParts[0])) {
        cleaned = commaParts[0];
      }
      if (cleaned && (!city || cleaned.toLowerCase() !== city.toLowerCase())) {
        landmark = cleaned;
      }
    }
  }

  return { landmark, event };
}

// ---------------------------------------------------------------------------
// 1. Sub-View History Back-Stack (prevents Android/Browser Back from exiting app)
// ---------------------------------------------------------------------------
const _subViewStack = [];
let _subViewListenerInstalled = false;
let _subViewSeq = 0;

function _ensureSubViewPopListener() {
  if (_subViewListenerInstalled || typeof window === "undefined") return;
  _subViewListenerInstalled = true;

  window.__omdPopSubView = () => {
    if (_subViewStack.length === 0) return false;
    const top = _subViewStack.pop();
    window.__omdSubViewJustPopped = true;
    setTimeout(() => {
      window.__omdSubViewJustPopped = false;
    }, 150);
    if (window.history.state?.omdSubView === top.id) {
      try {
        window.history.back();
      } catch (_) {}
    }
    try {
      top.onPop();
    } catch (err) {
      console.warn("[media-albums] subView onPop error:", err);
    }
    return true;
  };

  window.addEventListener(
    "popstate",
    (ev) => {
      if (_subViewStack.length === 0) return;
      const top = _subViewStack[_subViewStack.length - 1];
      const curStateId = ev?.state?.omdSubView || "";
      // If current history state still has our top token, nothing popped from us
      if (curStateId === top.id) return;

      _subViewStack.pop();
      window.__omdSubViewJustPopped = true;
      setTimeout(() => {
        window.__omdSubViewJustPopped = false;
      }, 150);

      try {
        top.onPop();
      } catch (err) {
        console.warn("[media-albums] subView onPop error:", err);
      }
    },
    true
  );
}

/**
 * Push an in-applet sub-view (opened Auto-Album, Story subfolder, or Scoped Viewer)
 * onto browser/WebView history so pressing hardware Back returns to parent view.
 */
export function pushSubViewHistory(label, onPop) {
  if (typeof window === "undefined" || typeof onPop !== "function") return "";
  _ensureSubViewPopListener();
  const id = `sv_${++_subViewSeq}_${Date.now()}`;
  _subViewStack.push({ id, label, onPop });
  try {
    const prev = window.history.state && typeof window.history.state === "object" ? window.history.state : {};
    window.history.pushState({ ...prev, omdSubView: id }, "", window.location.href);
  } catch (_) {}
  return id;
}

/**
 * Dismiss a sub-view initiated by an explicit UI close/back button (or component destroy).
 * If `popHistory` is true and this entry is currently on top of window.history.state,
 * pops the browser history entry cleanly without re-running `onPop`.
 */
export function dismissSubViewHistory(token, { popHistory = true } = {}) {
  if (!token || typeof window === "undefined") return;
  const idx = _subViewStack.findIndex((x) => x.id === token);
  if (idx !== -1) {
    _subViewStack.splice(idx, 1);
  }
  if (popHistory && window.history.state?.omdSubView === token) {
    window.__omdSubViewJustPopped = true;
    setTimeout(() => {
      window.__omdSubViewJustPopped = false;
    }, 150);
    try {
      window.history.back();
    } catch (_) {}
  }
}

// ---------------------------------------------------------------------------
// 2. Capture Date & Filename Timestamp Extraction
// ---------------------------------------------------------------------------
export function parseExifOrIsoTimestamp(rawStr) {
  if (!rawStr) return 0;
  if (rawStr instanceof Date) {
    const t = rawStr.getTime();
    return !isNaN(t) && t > 0 ? t : 0;
  }
  if (typeof rawStr === "number") {
    const ms = rawStr < 1e11 ? rawStr * 1000 : rawStr;
    return !isNaN(ms) && ms > 0 ? ms : 0;
  }
  const s = String(rawStr).trim();
  if (!s) return 0;
  const mExif = s.match(/^(\d{4})[:\-](\d{2})[:\-](\d{2})(?:[ T](\d{2}):(\d{2})(?::(\d{2}))?)?/);
  if (mExif) {
    const y = parseInt(mExif[1], 10);
    const mo = parseInt(mExif[2], 10) - 1;
    const d = parseInt(mExif[3], 10);
    const hh = parseInt(mExif[4] || "12", 10);
    const mm = parseInt(mExif[5] || "0", 10);
    const ss = parseInt(mExif[6] || "0", 10);
    if (y >= 1970 && y <= 2100 && mo >= 0 && mo <= 11 && d >= 1 && d <= 31) {
      const dt = new Date(y, mo, d, hh, mm, ss).getTime();
      if (!isNaN(dt) && dt > 0) return dt;
    }
  }
  const num = Number(s);
  if (!isNaN(num) && num > 0) {
    return num < 1e11 ? num * 1000 : num;
  }
  const parsed = new Date(s).getTime();
  return !isNaN(parsed) && parsed > 0 ? parsed : 0;
}

export function extractFilenameTimestamp(nameOrPath) {
  const fname = String(nameOrPath || "").split("/").pop() || "";
  if (!fname) return 0;

  // 1. YYYYMMDD_HHMMSS or YYYYMMDD-HHMMSS (IMG_20261015_145535, Lark_KREA_20261118_113821, PXL_...)
  const mYmdHms = fname.match(/(?:^|\D)(20\d{2})(\d{2})(\d{2})[_-](\d{2})(\d{2})(\d{2})/);
  if (mYmdHms) {
    const dt = new Date(
      parseInt(mYmdHms[1], 10),
      parseInt(mYmdHms[2], 10) - 1,
      parseInt(mYmdHms[3], 10),
      parseInt(mYmdHms[4], 10),
      parseInt(mYmdHms[5], 10),
      parseInt(mYmdHms[6], 10)
    ).getTime();
    if (!isNaN(dt) && dt > 0) return dt;
  }

  // 2. YYYY-MM-DD[ _-HH-MM-SS] (Screenshot_2026-10-15-14-22-10)
  const mDash = fname.match(/(?:^|\D)(20\d{2})-(\d{2})-(\d{2})(?:[_ T-](\d{2})[-.:](\d{2})(?:[-.:](\d{2}))?)?/);
  if (mDash) {
    const dt = new Date(
      parseInt(mDash[1], 10),
      parseInt(mDash[2], 10) - 1,
      parseInt(mDash[3], 10),
      parseInt(mDash[4] || "12", 10),
      parseInt(mDash[5] || "0", 10),
      parseInt(mDash[6] || "0", 10)
    ).getTime();
    if (!isNaN(dt) && dt > 0) return dt;
  }

  // 3. 13-digit epoch ms (e.g. krea_1794930113223.png)
  const mEpoch = fname.match(/(?:^|\D)(1[6-9]\d{11})(?:\D|$)/);
  if (mEpoch) {
    const ms = parseInt(mEpoch[1], 10);
    if (!isNaN(ms) && ms > 0) return ms;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// 3. Creations Metadata, Model, Persona & Scene Category Inference
// ---------------------------------------------------------------------------
const SCENE_CATEGORY_RULES = [
  { tag: "portrait", rx: /\b(close[- ]up portrait|medium close[- ]up|headshot|selfie|portrait of)\b/i },
  { tag: "cafe & bistro", rx: /\b(bistro|cafe|coffee shop|espresso|restaurant|bar|booth|wine glass)\b/i },
  { tag: "street & city", rx: /\b(city street|cobblestone|alley|sidewalk|crosswalk|downtown|urban|street lamps|balcony)\b/i },
  { tag: "interior", rx: /\b(bookshop|bookshelf|library|elevator|bedroom|living room|hotel|apartment|kitchen|studio loft)\b/i },
  { tag: "nightlife", rx: /\b(nightclub|gothic club|club|neon lights|party|concert|lounge)\b/i },
  { tag: "nature & outdoors", rx: /\b(beach|ocean|sea|mountain|mountains|forest|woods|garden|park|lake|lavender|meadow)\b/i },
  { tag: "golden hour", rx: /\b(golden hour|sunset|sunrise|sunlit|warm tungsten|candlelight)\b/i },
  { tag: "cinematic & film", rx: /\b(35mm|film grain|analog|cinematic|bokeh|50mm|85mm|volumetric lighting)\b/i },
];

export function humanizeCheckpointName(rawCkpt = "") {
  const stem = String(rawCkpt || "")
    .trim()
    .replace(/\.(safetensors|ckpt|pt|pth|bin)$/i, "")
    .replace(/_+\d+$/, "")
    .trim();
  if (!stem) return "";
  const low = stem.toLowerCase();

  if (low.includes("analogmadness")) return { key: "ckpt:analog_madness_sdxl", label: "AnalogMadness SDXL" };
  if (low.includes("cyberrealisticxl") || low === "realistic") return { key: "ckpt:cyberrealistic_xl", label: "CyberRealistic XL" };
  if (low.includes("ponydiffusion") || low.includes("pony_diffusion") || low === "tooned") {
    return { key: "ckpt:pony_diffusion_v6_xl", label: "Pony Diffusion V6 XL" };
  }
  if (low.includes("ponysemi")) return { key: "ckpt:pony_semi_xl", label: "Pony Semi XL" };
  if (low.includes("cyberrealisticpony") || low.includes("ponyrealism") || low === "perfect") {
    return { key: "ckpt:cyberrealistic_pony", label: "CyberRealistic Pony" };
  }
  if (low.includes("illustrious") || low === "fantasy") return { key: "ckpt:illustrious_xl", label: "Illustrious XL" };
  if (low.includes("juggernaut")) return { key: "ckpt:juggernaut_xl", label: "Juggernaut XL" };
  if (low.includes("albedobase") || low.includes("albedo_base")) return { key: "ckpt:albedobase_xl", label: "AlbedoBase XL" };
  if (low.includes("flux")) return { key: "ckpt:flux", label: "Flux.1" };
  if (low.includes("krea")) return { key: "krea2", label: "Krea 2" };
  if (low.includes("anima")) return { key: "anima", label: "Anima" };
  if (low.includes("minimax") || low === "h3") return { key: "video_h3", label: "MiniMax-H3 · Video" };
  if (low.includes("horde")) return { key: "horde", label: "AI Horde" };

  // Generic humanizer for checkpoint filenames like myModelSDXL_v20 -> MyModel SDXL
  const cleaned = stem
    .replace(/[_-]+(?:v|xl|fp16|fp8|bf16|pruned)[0-9a-z.]*$/i, "")
    .replace(/([a-z])([A-Z])/g, "$1 $2")
    .replace(/[_-]+/g, " ")
    .replace(/\s+/g, " ")
    .trim();
  const pretty = cleaned
    ? cleaned.charAt(0).toUpperCase() + cleaned.slice(1)
    : stem;
  const slug = pretty.toLowerCase().replace(/[^a-z0-9]+/g, "_").replace(/^_+|_+$/g, "");
  return { key: `ckpt:${slug || "custom"}`, label: pretty };
}

export function isLikelyGeneratedMedia({
  filename = "",
  path = "",
  text = "",
  tags = [],
  defaultStorage = "",
} = {}) {
  const cleanPath = String(path || "").toLowerCase();
  const fname = String(filename || cleanPath.split("/").pop() || "");
  const lowName = fname.toLowerCase();
  const txt = String(text || "");

  if (cleanPath.includes("/onmychat/") || cleanPath.includes("/creations/") || cleanPath.includes("/generated/")) {
    return true;
  }
  if (defaultStorage) {
    const cleanDef = String(defaultStorage).replace(/^\/+|\/+$/g, "").toLowerCase();
    if (cleanDef && cleanDef.includes("onmychat") && cleanPath.includes(cleanDef)) return true;
  }
  if (/^(krea_|anima_|horde_|gen_|sdxl_|h3_|comfyui_|avatar_)/i.test(fname)) return true;
  if (/^[a-zа-яё]{2,20}(?:avatar)?_(?:krea|sdxl|h3|anima|horde)_/i.test(fname)) return true;
  if (/^img_\d{5}_/i.test(lowName) || /\.safetensors_\d+/i.test(lowName) || /^img-[a-z0-9_]+_\d+_/i.test(lowName)) {
    return true;
  }
  if (/<!--\s*(?:prompt|model):/i.test(txt)) return true;
  if (/\bscore_9\s*,\s*score_8_up\b/i.test(txt)) return true;
  const tagsLow = Array.isArray(tags) ? tags.map((t) => String(t || "").toLowerCase().replace(/^#+/, "")) : [];
  if (tagsLow.includes("ai_generated") || tagsLow.includes("krea") || tagsLow.includes("sdxl") || tagsLow.includes("comfyui")) {
    return true;
  }
  return false;
}

export function detectCreationModelInfo(filename, isVideo = false, extra = {}) {
  const fname = String(filename || "");
  const low = fname.toLowerCase();
  const modelHint = String(extra?.modelHint || "").trim();
  const combinedTxt = `${extra?.prompt || ""}\n${extra?.rawText || ""}`;

  if (modelHint) {
    const parsedHint = humanizeCheckpointName(modelHint);
    if (parsedHint) return parsedHint;
  }

  // 1. Embedded checkpoint filename like IMG-analogMadnessSDXL_xl5.safetensors_00644_.png
  const mCkpt = fname.match(/^(?:IMG-|GEN-|SDXL-)?([A-Za-z0-9_.-]+?)\.safetensors(?:_\d+)?/i);
  if (mCkpt && mCkpt[1]) {
    const parsedCkpt = humanizeCheckpointName(mCkpt[1]);
    if (parsedCkpt) return parsedCkpt;
  }

  // 2. Avatars
  if (/^avatar_/i.test(fname) || /avatar_(?:krea|sdxl)/i.test(low) || /^[a-z]+avatar_/i.test(fname)) {
    return { key: "avatars", label: "Avatars" };
  }
  // 3. Video / MiniMax-H3
  if (/\bh3[_-]/i.test(fname) || /_h3_/i.test(low) || isVideo) {
    return { key: "video_h3", label: "MiniMax-H3 · Video" };
  }
  // 4. Krea 2
  if (/\bkrea\b|_krea_|^krea_/i.test(fname)) {
    return { key: "krea2", label: "Krea 2" };
  }
  // 5. AI Horde
  if (/^horde_|_horde_/i.test(low)) {
    return { key: "horde", label: "AI Horde" };
  }
  // 6. Anima
  if (/^anima_|_anima_/i.test(low)) {
    return { key: "anima", label: "Anima" };
  }
  // 7. Pony / Illustrious prompt tags in raw prompt sidecars (e.g. score_9, score_8_up)
  if (/\bscore_9\s*,\s*score_8_up\b/i.test(combinedTxt)) {
    return { key: "ckpt:pony_xl", label: "Pony XL" };
  }
  // 8. ComfyUI direct workflow outputs
  if (/^comfyui_/i.test(low)) {
    return { key: "comfyui", label: "ComfyUI Studio" };
  }
  // 9. Express SDXL Studio
  if (/^sdxl_|^img_\d{5}_|^gen_/i.test(low)) {
    return { key: "sdxl", label: "SDXL Studio" };
  }
  return { key: "studio", label: "Studio" };
}

export function parseReadmeSidecarText(rawTxt = "") {
  const txt = String(rawTxt || "").trim();
  if (!txt) {
    return { title: "", description: "", prompt: "", model: "", isSensitive: false, explicitTags: [] };
  }

  let prompt = "";
  const mPrompt = txt.match(/<!--\s*prompt:\s*([\s\S]*?)-->/i);
  if (mPrompt) {
    prompt = mPrompt[1].trim();
  }

  let model = "";
  const mModel = txt.match(/<!--\s*model:\s*([\s\S]*?)-->/i);
  if (mModel) {
    model = mModel[1].trim();
  }

  const withoutComments = txt.replace(/<!--[\s\S]*?-->/g, "").trim();
  let title = "";
  const mTitle = withoutComments.match(/^#\s+([^\n]+)/);
  if (mTitle) {
    title = mTitle[1].trim();
  }

  const body = withoutComments.replace(/^#\s+[^\n]+\n*/, "").trim();
  // Legacy raw-prompt sidecars (e.g. "<Judith> score_9, score_8_up...") without markdown title
  if (!prompt && !title && (/\bscore_9\b/i.test(body) || /^<[A-Za-z0-9_-]{2,24}>/.test(body))) {
    prompt = body;
  }

  const explicitTags = [];
  const seen = new Set();
  const tagMatches = body.matchAll(/(?:^|\s)#([a-zA-Zа-яА-ЯёЁіїєґІЇЄҐ0-9_]{2,30})/g);
  for (const m of tagMatches) {
    const t = m[1].toLowerCase();
    if (t === "sensitive" || t === "nsfw") continue;
    if (!NOISE_MEDIA_TAGS.has(t) && !seen.has(t)) {
      seen.add(t);
      explicitTags.push(t);
    }
  }

  const cleanDesc = body
    .split("\n")
    .map((l) => l.replace(/(?:^|\s)#(?:sensitive|nsfw)\b/gi, "").trim())
    .filter(Boolean)
    .join(" ")
    .trim();

  const isSensitive = Boolean(hasSensitiveTag(txt) || hasSensitiveReadmeContent(txt));
  return {
    title,
    description: cleanDesc,
    prompt,
    model,
    isSensitive,
    explicitTags,
  };
}

export function enrichCreationAttributes({
  filename = "",
  path = "",
  title = "",
  description = "",
  prompt = "",
  modelHint = "",
  rawText = "",
  personaHint = "",
  storyFolder = "",
  isVideo = false,
  explicitTags = [],
  knownPersonas = [],
}) {
  const fname = String(filename || path.split("/").pop() || "");
  const modelInfo = detectCreationModelInfo(fname, isVideo, {
    modelHint,
    prompt,
    rawText: rawText || description,
  });

  // Build lookup of known personas
  const personaMap = new Map();
  for (const p of knownPersonas || []) {
    if (p && p.name) {
      personaMap.set(p.name.trim().toLowerCase(), p.name.trim());
    }
  }

  const matchedPersonas = [];
  const addPersona = (rawName) => {
    const clean = String(rawName || "").trim();
    if (!clean || clean.length < 2 || clean.length > 28) return;
    const low = clean.toLowerCase();
    if (["img", "sdxl", "krea", "h3", "clip", "avatar", "video", "photo", "gen", "horde", "anima", "scene"].includes(low)) {
      return;
    }
    if (/^\d+$/.test(low)) return;
    const canonical = personaMap.get(low) || (clean.charAt(0).toUpperCase() + clean.slice(1));
    if (!matchedPersonas.some((x) => x.toLowerCase() === canonical.toLowerCase())) {
      matchedPersonas.push(canonical);
    }
  };

  if (personaHint) addPersona(personaHint);

  // 1. Filename prefix like Lark_KREA_..., LarkAvatar_KREA_..., Dana_SDXL_...
  const mPrefix = fname.match(/^([A-Za-zА-Яа-яЁё]{2,20}?)(?:Avatar)?_(?:KREA|SDXL|H3|ANIMA|HORDE)\b/i);
  if (mPrefix) {
    addPersona(mPrefix[1]);
  }

  const combinedText = `${title}\n${description}\n${prompt}`;

  // 2. Known personas mentioned in title/description/prompt
  for (const [lowName, canonicalName] of personaMap.entries()) {
    const escaped = lowName.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
    const rx = new RegExp(`(?:^|[^a-zA-Zа-яА-Я0-9_])${escaped}(?:$|[^a-zA-Zа-яА-Я0-9_])`, "i");
    if (rx.test(combinedText)) {
      addPersona(canonicalName);
    }
  }

  // 3. Explicit character patterns in prompt: "shot of Kate, a 23-year-old..." or "<Judith>" / "<lora:Name:...>"
  const mAngleTag = combinedText.match(/(?:^|\s)<([A-Z][a-zA-Z0-9]{1,18})>/);
  if (mAngleTag && !["Picture", "Image", "Video", "Photo", "Scene"].includes(mAngleTag[1])) {
    addPersona(mAngleTag[1]);
  }
  const mShotOf = prompt.match(/\b(?:shot|portrait|photo|view) of ([A-Z][a-z]{2,15})(?:\s*,|\s+pulling|\s+standing|\s+sitting|\s+leaning|\s+walking|\s+wearing|\s+in\b)/);
  if (mShotOf && !["The", "Her", "His", "Two", "One"].includes(mShotOf[1])) {
    addPersona(mShotOf[1]);
  }

  // Scene categories + explicit tags
  const tags = [];
  const seenTags = new Set();
  const pushTag = (t) => {
    const clean = String(t || "").trim().toLowerCase().replace(/^#+/, "");
    if (!clean || clean.length < 2 || NOISE_MEDIA_TAGS.has(clean) || seenTags.has(clean)) return;
    seenTags.add(clean);
    tags.push(clean);
  };

  for (const t of explicitTags || []) pushTag(t);
  for (const rule of SCENE_CATEGORY_RULES) {
    if (rule.rx.test(combinedText)) {
      pushTag(rule.tag);
    }
  }

  // Detect story folder from path if not explicitly passed
  let resolvedStory = storyFolder || "";
  if (!resolvedStory && path) {
    const mStory = String(path).match(/\/generated\/([^\/]+)\/[^\/]+$/i);
    if (
      mStory &&
      mStory[1] &&
      !mStory[1].startsWith(".") &&
      !/\.readme\.md$/i.test(mStory[1]) &&
      !/\.(md|txt|json|png|jpe?g|webp|gif|mp4|mov|webm)$/i.test(mStory[1])
    ) {
      resolvedStory = mStory[1];
    }
  }

  return {
    modelKey: modelInfo.key,
    modelLabel: modelInfo.label,
    personas: matchedPersonas,
    storyFolder: resolvedStory,
    semanticTags: tags,
  };
}

// ---------------------------------------------------------------------------
// 4. Lightweight Readme Cache & Direct Creations Scanner (for MediaLibrary)
// ---------------------------------------------------------------------------
const README_META_CACHE_KEY = "omd_media_readme_meta_v2";
let _memReadmeCache = null;

function _loadReadmeCache() {
  if (_memReadmeCache) return _memReadmeCache;
  _memReadmeCache = {};
  if (typeof localStorage === "undefined") return _memReadmeCache;
  try {
    const raw = localStorage.getItem(README_META_CACHE_KEY);
    if (raw) _memReadmeCache = JSON.parse(raw) || {};
  } catch (_) {
    _memReadmeCache = {};
  }
  return _memReadmeCache;
}

function _saveReadmeCache() {
  if (!_memReadmeCache || typeof localStorage === "undefined") return;
  try {
    const keys = Object.keys(_memReadmeCache);
    if (keys.length > 1500) {
      const trimmed = {};
      for (const k of keys.slice(keys.length - 1200)) {
        trimmed[k] = _memReadmeCache[k];
      }
      _memReadmeCache = trimmed;
    }
    localStorage.setItem(README_META_CACHE_KEY, JSON.stringify(_memReadmeCache));
  } catch (_) {}
}

async function _readDriveTextSafe(drive, httpDrive, path) {
  if (drive) {
    try {
      if (typeof drive.homeRead === "function") {
        const b64 = await drive.homeRead(path).catch(() => null);
        if (b64) return decodeURIComponent(escape(atob(b64)));
      }
      if (typeof drive.readFile === "function") {
        const res = await drive.readFile(path).catch(() => null);
        const b64 = res && res.data ? res.data : (typeof res === "string" ? res : null);
        if (b64) return new TextDecoder("utf-8").decode(Uint8Array.from(atob(b64.replace(/\s+/g, "")), (c) => c.charCodeAt(0)));
      }
    } catch (_) {}
  }
  if (httpDrive && typeof httpDrive.fetch === "function") {
    try {
      const resp = await httpDrive.fetch(path).catch(() => null);
      if (resp && resp.ok) {
        const ct = (resp.headers?.get?.("content-type") || "").toLowerCase();
        if (!ct.includes("text/html")) {
          return await resp.text();
        }
      }
    } catch (_) {}
  }
  return "";
}

export function resolveCreationsBaseStorage(defaultStorage = "", userName = "") {
  let raw = (
    defaultStorage ||
    (typeof localStorage !== "undefined" ? localStorage.getItem("omd_default_storage") : "") ||
    (typeof window !== "undefined" && window.__omdProfile?.defaultStorage) ||
    DEFAULT_STORAGE_PATH ||
    "/Data"
  ).trim();
  const { folder, device, deviceName } = parseStoragePath(raw);
  let clean = (folder || "").replace(/^\/+|\/+$/g, "");

  const knownUsers = [
    userName,
    typeof window !== "undefined" && window.__omdProfile?.user,
    typeof window !== "undefined" && window.__omdProfile?.name,
  ].filter(Boolean);

  for (const u of knownUsers) {
    const uLow = String(u).toLowerCase();
    if (clean.toLowerCase() === uLow) {
      clean = (DEFAULT_STORAGE_PATH || "/Data").replace(/^\/+/, "");
      break;
    } else if (clean.toLowerCase().startsWith(uLow + "/")) {
      clean = clean.slice(String(u).length).replace(/^\/+/, "");
    }
  }

  clean = clean.replace(/^(home|Users)\/[^\/]+(\/|$)/i, "").replace(/^\/+|\/+$/g, "");
  clean = clean.replace(/^private\//i, "");
  if (clean.endsWith("/generated")) clean = clean.slice(0, -"/generated".length);
  else if (clean === "generated") clean = "";
  clean = clean.replace(/^\/+|\/+$/g, "");
  if (!clean || clean.toLowerCase() === "alexey") {
    clean = (DEFAULT_STORAGE_PATH || "/Data").replace(/^\/+/, "");
  }
  return {
    baseFolder: clean || "Data",
    device,
    deviceName,
  };
}

/**
 * Directly scans the `generated/` directory (plus story subfolders and `personas/*.json`)
 * so unindexed Creations appear in Media Library with full Persona, Story, Model, and Tag albums.
 */
export async function scanDirectCreationsFolder({
  deviceDrive = null,
  storageDevice = null,
  targetDevice = null,
  httpDrive = null,
  defaultStorage = "",
  userName = "",
  onBatchUpdate = null,
  signal = null,
}) {
  const { baseFolder, device: parsedDev } = resolveCreationsBaseStorage(defaultStorage, userName);
  let activeDev =
    parsedDev ||
    storageDevice ||
    (deviceDrive && deviceRegistry.list().find((d) => d.drive === deviceDrive && !d.isAiNode)) ||
    (targetDevice && !targetDevice.isAiNode ? targetDevice : null) ||
    deviceRegistry.list().find((d) => !d.isAiNode && d.drive && d.drive.isConnected) ||
    null;

  let activeDrive = (activeDev && activeDev.drive) || deviceDrive || null;
  if (activeDrive && !activeDrive.isConnected && typeof activeDrive.waitUntilConnected === "function") {
    await activeDrive.waitUntilConnected(2000).catch(() => {});
  }

  const candidateFolders = [
    `/${baseFolder}/generated`,
    "/OnMyChat/generated",
    "/Private/OnMyChat/generated",
    "/Data/generated",
    "/Private/Data/generated",
    "/generated",
  ].filter((v, i, a) => v && a.indexOf(v) === i);

  const listDir = async (dirPath) => {
    if (activeDrive && typeof activeDrive.getFiles === "function" && (activeDrive.isConnected || activeDrive.status === "connected" || activeDev?.local)) {
      try {
        const res = await activeDrive.getFiles(dirPath);
        if (Array.isArray(res) && res.length > 0) return res;
      } catch (_) {}
    }
    if (httpDrive && typeof httpDrive.fetch === "function") {
      try {
        const res = await httpDrive.fetch(`/api/v1/files?path=${encodeURIComponent(dirPath)}`);
        if (res && res.ok) {
          const data = await res.json();
          const arr = data.list || data.files || [];
          if (Array.isArray(arr) && arr.length > 0) return arr;
        }
      } catch (_) {}
    }
    return [];
  };

  let rootEntries = [];
  let effectiveFolder = candidateFolders[0];
  for (const cand of candidateFolders) {
    if (signal?.aborted) return { items: [], personas: [] };
    const entries = await listDir(cand);
    if (entries.length > 0) {
      rootEntries = entries;
      effectiveFolder = cand;
      break;
    }
  }

  if (!rootEntries.length) {
    return { items: [], personas: [] };
  }

  const devPrefix = (
    activeDev?.prefix ||
    activeDev?.name ||
    activeDev?.linkId ||
    targetDevice?.prefix ||
    targetDevice?.name ||
    targetDevice?.linkId ||
    "local"
  ).replace(/^\/+|\/+$/g, "");

  // 1. Load personas/*.json from sibling folder for real names & avatars
  const personasFolder = effectiveFolder.replace(/\/generated\/?$/i, "/personas");
  const personas = [];
  if (personasFolder && personasFolder !== effectiveFolder) {
    const pEntries = await listDir(personasFolder);
    const jsonFiles = pEntries.filter((f) => !f.isDir && /\.json$/i.test(f.name || f.value || ""));
    for (const jf of jsonFiles.slice(0, 30)) {
      if (signal?.aborted) break;
      const jName = jf.name || jf.value;
      const rawJson = await _readDriveTextSafe(activeDrive, httpDrive, `${personasFolder}/${jName}`);
      if (rawJson) {
        try {
          const pObj = JSON.parse(rawJson);
          if (pObj && pObj.name) {
            personas.push({
              id: pObj.id || jName,
              name: pObj.name,
              avatar_b64: pObj.avatar || "",
            });
          }
        } catch (_) {}
      }
    }
  }

  // 2. Collect media files in generated/ AND immediate story subfolders
  const allFileRecords = [];
  const collectFromList = (entries, folderPath, storyFolder = "") => {
    const readmeSet = new Set();
    for (const e of entries) {
      const nm = e.name || e.value || "";
      if (!e.isDir && /\.readme\.md$/i.test(nm)) {
        readmeSet.add(nm.replace(/\.readme\.md$/i, "").toLowerCase());
      }
    }
    for (const e of entries) {
      const nm = e.name || e.value || "";
      if (!nm || nm.startsWith(".") || e.isDir || e.type === "folder") continue;
      if (nm === "folder.jpg" || /\.cover\.(jpe?g|png)$/i.test(nm)) continue;
      const ext = nm.includes(".") ? nm.split(".").pop().toLowerCase() : "";
      if (!IMAGE_EXTS.has(ext) && !VIDEO_EXTS.has(ext)) continue;
      const relPath = `${folderPath}/${nm}`.replace(/\/+/g, "/");
      const docId = `/${devPrefix}${relPath}`;
      const stem = nm.replace(/\.[^/.]+$/, "").toLowerCase();
      allFileRecords.push({
        entry: e,
        name: nm,
        ext,
        isVideo: VIDEO_EXTS.has(ext),
        relPath,
        docId,
        storyFolder,
        hasReadme: readmeSet.has(stem),
      });
    }
  };

  collectFromList(rootEntries, effectiveFolder, "");

  const subDirs = rootEntries.filter((e) => {
    const nm = e.name || e.value || "";
    return nm && !nm.startsWith(".") && !/\.readme\.md$/i.test(nm) && (e.isDir || e.type === "folder");
  });

  for (const dir of subDirs.slice(0, 25)) {
    if (signal?.aborted) break;
    const subName = dir.name || dir.value;
    const subPath = `${effectiveFolder}/${subName}`.replace(/\/+/g, "/");
    const subEntries = await listDir(subPath);
    if (subEntries.length > 0) {
      collectFromList(subEntries, subPath, subName);
    }
  }

  const readmeCache = _loadReadmeCache();
  const buildCardFromRecord = (rec, meta) => {
    const mtimeMs =
      extractFilenameTimestamp(rec.name) ||
      parseExifOrIsoTimestamp(rec.entry.lastModified || rec.entry.date || rec.entry.mtime) ||
      0;
    const enriched = enrichCreationAttributes({
      filename: rec.name,
      path: rec.relPath,
      title: meta?.title || "",
      description: meta?.description || "",
      prompt: meta?.prompt || "",
      modelHint: meta?.model || "",
      storyFolder: rec.storyFolder,
      isVideo: rec.isVideo,
      explicitTags: meta?.explicitTags || [],
      knownPersonas: personas,
    });

    return {
      id: `creation:${rec.docId}`,
      document_id: rec.docId,
      path: rec.docId,
      relPath: rec.relPath,
      title: meta?.title || rec.name,
      text: [
        meta?.description,
        meta?.model ? `<!-- model: ${meta.model} -->` : "",
        meta?.prompt ? `<!-- prompt: ${meta.prompt} -->` : "",
      ].filter(Boolean).join("\n"),
      contentType: rec.isVideo ? "video" : "image",
      isDirectCreation: true,
      onDevice: Boolean(!activeDev?.local),
      source: "creations",
      sensitive: Boolean(meta?.isSensitive),
      tags: [
        ...(meta?.isSensitive ? ["sensitive"] : []),
        ...enriched.semanticTags,
      ],
      taken_at: mtimeMs,
      created: mtimeMs,
      storyFolder: enriched.storyFolder,
      modelKey: enriched.modelKey,
      modelLabel: enriched.modelLabel,
      matchedPersonas: enriched.personas,
    };
  };

  const builtCards = allFileRecords.map((rec) => {
    const sig = `${rec.relPath}:${rec.entry.size || 0}`;
    const cached = readmeCache[sig] || null;
    return buildCardFromRecord(rec, cached);
  });

  if (typeof onBatchUpdate === "function") {
    onBatchUpdate({ items: builtCards, personas });
  }

  // 3. Background hydrate uncached .Readme.md files in small batches (newest first)
  const uncachedIndices = [];
  for (let i = 0; i < allFileRecords.length; i++) {
    const rec = allFileRecords[i];
    if (!rec.hasReadme) continue;
    const sig = `${rec.relPath}:${rec.entry.size || 0}`;
    if (!readmeCache[sig]) {
      uncachedIndices.push(i);
    }
  }

  if (uncachedIndices.length > 0 && !signal?.aborted) {
    // Sort uncached indices newest-first so visible items hydrate immediately
    uncachedIndices.sort((ia, ib) => (builtCards[ib].taken_at || 0) - (builtCards[ia].taken_at || 0));
    let dirtyCount = 0;
    const BATCH = 6;
    for (let b = 0; b < uncachedIndices.length; b += BATCH) {
      if (signal?.aborted) break;
      const slice = uncachedIndices.slice(b, b + BATCH);
      await Promise.all(
        slice.map(async (idx) => {
          const rec = allFileRecords[idx];
          const readmePath = rec.relPath.replace(/\.[^/.]+$/, "") + ".Readme.md";
          const txt = await _readDriveTextSafe(activeDrive, httpDrive, readmePath);
          const sig = `${rec.relPath}:${rec.entry.size || 0}`;
          const parsed = parseReadmeSidecarText(txt || "");
          readmeCache[sig] = parsed;
          builtCards[idx] = buildCardFromRecord(rec, parsed);
          dirtyCount++;
        })
      );
      if (dirtyCount >= 18 && typeof onBatchUpdate === "function" && !signal?.aborted) {
        dirtyCount = 0;
        _saveReadmeCache();
        onBatchUpdate({ items: [...builtCards], personas });
      }
    }
    _saveReadmeCache();
    if (typeof onBatchUpdate === "function" && !signal?.aborted) {
      onBatchUpdate({ items: [...builtCards], personas });
    }
  }

  return { items: builtCards, personas };
}

// ---------------------------------------------------------------------------
// 5. Shared Timeline Day Groups & Year-Month Scrubber Builders
// ---------------------------------------------------------------------------
export function buildTimelineDayGroups(items, { localeStr = undefined, undatedLabel = "Undated" } = {}) {
  const groups = [];
  let cur = null;

  for (const m of items || []) {
    const ts = Number(m.takenMs || 0);
    let dayKey = "undated";
    let monthKey = "undated";
    let dayLabel = undatedLabel;
    let monthShort = "•";

    if (ts > 0) {
      const dt = new Date(ts);
      const y = dt.getFullYear();
      const mo = String(dt.getMonth() + 1).padStart(2, "0");
      const d = String(dt.getDate()).padStart(2, "0");
      dayKey = `${y}-${mo}-${d}`;
      monthKey = `${y}-${mo}`;
      dayLabel = dt.toLocaleDateString(localeStr, {
        day: "numeric",
        month: "long",
        year: "numeric",
        weekday: "short",
      });
      monthShort = dt.toLocaleDateString(localeStr, { month: "short", year: "2-digit" });
    }

    if (!cur || cur.dayKey !== dayKey) {
      cur = {
        dayKey,
        monthKey,
        monthShort,
        dayLabel,
        placesCount: new Map(),
        topPlace: "",
        items: [],
      };
      groups.push(cur);
    }
    cur.items.push(m);
    const placeName = m.city || m.place || "";
    if (placeName) {
      cur.placesCount.set(placeName, (cur.placesCount.get(placeName) || 0) + 1);
    }
  }

  for (const g of groups) {
    let bestP = "";
    let bestCnt = 0;
    for (const [p, cnt] of g.placesCount.entries()) {
      if (cnt > bestCnt) {
        bestP = p;
        bestCnt = cnt;
      }
    }
    g.topPlace = bestP;
  }

  return groups;
}

export function buildMonthScrubberMarkers(allSortedItems, localeStr = undefined) {
  const seen = new Map();
  for (let i = 0; i < (allSortedItems || []).length; i++) {
    const m = allSortedItems[i];
    const ts = Number(m.takenMs || 0);
    if (!ts) continue;
    const dt = new Date(ts);
    const y = dt.getFullYear();
    const mo = String(dt.getMonth() + 1).padStart(2, "0");
    const key = `${y}-${mo}`;
    if (!seen.has(key)) {
      seen.set(key, {
        key,
        year: y,
        shortLabel: dt.toLocaleDateString(localeStr, { month: "short" }),
        fullLabel: dt.toLocaleDateString(localeStr, { month: "long", year: "numeric" }),
        firstIndex: i,
      });
    }
  }
  return Array.from(seen.values());
}
