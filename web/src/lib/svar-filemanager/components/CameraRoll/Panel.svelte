<script>
	import { getContext, onMount, onDestroy, tick } from "svelte";
	import { delegateClick, setID } from "@svar-ui/lib-dom";
	import Item from "./Item.svelte";
	import StoryComicView from "../../../../components/StoryComicView.svelte";
	import { isImageOrVideo, isVideoFile, isIgnoredFile } from "./media-utils.js";
	import {
		isStoryOrLongReadme,
		loadFolderStoryReadme,
		resolveFolderMediaAssetUrl,
	} from "../../../story-composer.js";
	import { _ as i18nT, locale as i18nLocale } from "../../../i18n/index.js";
	import { contentModeStore, hasSensitiveTag } from "../../../content-mode.js";
	import {
		pushSubViewHistory,
		dismissSubViewHistory,
		extractFilenameTimestamp,
		parseExifOrIsoTimestamp,
		detectCreationModelInfo,
		isLikelyGeneratedMedia,
		parseReadmeSidecarText,
		canonicalizePlaceKey,
		filterAndCanonicalizePhotoTags,
		extractCardLandmarkAndEvent,
		buildTimelineDayGroups,
		buildMonthScrubberMarkers,
	} from "../../../media-albums.js";

	const api = getContext("filemanager-store");
	const _ = getContext("wx-i18n").getGroup("filemanager");
	const templates = api.templates;

	let { panels, activePanel, mode } = api.getReactiveState();

	const files = $derived($panels[$activePanel]._files);
	const selected = $derived($panels[$activePanel].selected);
	const path = $derived($panels[$activePanel].path);
	const crumbs = $derived($panels[$activePanel]._crumbs);

	let innerWidth = $state(typeof window !== "undefined" ? window.innerWidth : 1024);
	const isMobile = $derived(innerWidth < 768);

	let crTab = $state(
		typeof localStorage !== "undefined"
			? localStorage.getItem("omd_cr_tab") || "timeline"
			: "timeline"
	);
	let crSort = $state(
		typeof localStorage !== "undefined"
			? localStorage.getItem("omd_cr_sort") || "desc"
			: "desc"
	);
	let activeAlbum = $state(null);
	let albumBackToken = "";
	let mobileScrubberOpen = $state(false);
	let activeScrubberKey = $state("");
	let scrollEl = $state(null);
	let knowledgeTick = $state(0);
	let failedFolderCovers = $state({});
	let lastTrackedPath = "";
	let folderReadme = $state("");
	let localMediaVer = $state(0);
	let isDark = $state(
		typeof document !== "undefined"
			? document.documentElement.classList.contains("dark-theme")
			: true
	);

	const hasStoryComic = $derived(
		$mode !== "search" && isStoryOrLongReadme(folderReadme)
	);

	$effect(() => {
		const curPath = path || "";
		const curFiles = files || [];
		let cancelled = false;
		if (!curPath || curPath === "/" || $mode === "search") {
			folderReadme = "";
		} else {
			loadFolderStoryReadme(curPath, curFiles).then((txt) => {
				if (!cancelled) folderReadme = txt || "";
			});
		}
		return () => {
			cancelled = true;
		};
	});

	$effect(() => {
		const curPath = path || "";
		if (curPath !== lastTrackedPath) {
			lastTrackedPath = curPath;
			failedFolderCovers = {};
			if (activeAlbum) {
				activeAlbum = null;
				if (albumBackToken) {
					dismissSubViewHistory(albumBackToken, { popHistory: false });
					albumBackToken = "";
				}
			}
			mobileScrubberOpen = false;
		}
	});

	function onKnowledgeUpdated() {
		knowledgeTick += 1;
	}

	let themeObserver = null;

	onMount(() => {
		if (typeof window !== "undefined") {
			window.addEventListener("omd-knowledge-updated", onKnowledgeUpdated);
		}
		if (typeof document !== "undefined" && typeof MutationObserver !== "undefined") {
			isDark = document.documentElement.classList.contains("dark-theme");
			themeObserver = new MutationObserver(() => {
				isDark = document.documentElement.classList.contains("dark-theme");
			});
			themeObserver.observe(document.documentElement, { attributes: true, attributeFilter: ["class"] });
		}
	});

	onDestroy(() => {
		if (typeof window !== "undefined") {
			window.removeEventListener("omd-knowledge-updated", onKnowledgeUpdated);
		}
		if (themeObserver) {
			themeObserver.disconnect();
			themeObserver = null;
		}
		if (albumBackToken) {
			dismissSubViewHistory(albumBackToken, { popHistory: false });
			albumBackToken = "";
		}
	});

	function setCrTab(nextTab) {
		crTab = nextTab;
		if (nextTab === "timeline" && activeAlbum) {
			closeAlbum();
		}
		mobileScrubberOpen = false;
		if (typeof localStorage !== "undefined") {
			localStorage.setItem("omd_cr_tab", nextTab);
		}
	}

	async function toggleCrSort() {
		crSort = crSort === "desc" ? "asc" : "desc";
		if (typeof localStorage !== "undefined") {
			localStorage.setItem("omd_cr_sort", crSort);
		}
		await tick();
		if (scrollEl) {
			scrollEl.scrollTo({ top: 0, behavior: "smooth" });
		}
	}

	function openAlbum(alb) {
		const hadAlbum = Boolean(activeAlbum);
		activeAlbum = alb;
		mobileScrubberOpen = false;
		if (!hadAlbum) {
			if (albumBackToken) {
				dismissSubViewHistory(albumBackToken, { popHistory: false });
			}
			albumBackToken = pushSubViewHistory("cameraroll-album", () => {
				albumBackToken = "";
				activeAlbum = null;
				mobileScrubberOpen = false;
			});
		}
		if (scrollEl) {
			scrollEl.scrollTo({ top: 0, behavior: "auto" });
		}
	}

	function closeAlbum() {
		activeAlbum = null;
		mobileScrubberOpen = false;
		if (albumBackToken) {
			const tok = albumBackToken;
			albumBackToken = "";
			dismissSubViewHistory(tok, { popHistory: true });
		}
	}

	const realFiles = $derived((files || []).filter((f) => !isIgnoredFile(f)));
	const subfolders = $derived(realFiles.filter((f) => f.type === "folder" || f.isDir));
	const mediaFiles = $derived(realFiles.filter((f) => isImageOrVideo(f)));

	// Optional enrichment from indexed Knowledge Cards if available in memory
	const knowledgeLookup = $derived.by(() => {
		void knowledgeTick;
		const byPath = new Map();
		const byName = new Map();
		const list = (typeof window !== "undefined" && Array.isArray(window.__omdKnowledgeCards))
			? window.__omdKnowledgeCards
			: [];
		for (const k of list) {
			const docId = String(k?.document_id || k?.path || "");
			if (!docId) continue;
			byPath.set(docId.toLowerCase(), k);
			const fname = docId.split("/").pop()?.toLowerCase();
			if (fname && !byName.has(fname)) {
				byName.set(fname, k);
			}
		}
		return { byPath, byName };
	});

	function parseKnowledgePlace(k) {
		if (!k) return { city: "", country: "" };
		let raw = String(k.location || "").trim();
		const txt = String(k.text || k.content || "");
		if (!raw && txt) {
			const m = txt.match(/(?:^|\n)Location(?:\s*\([^)]*\))?:\s*([^\n.]+)/i);
			if (m) raw = m[1].trim();
		}
		if (!raw || /^[-+]?\d+\.\d+\s*,\s*[-+]?\d+\.\d+$/.test(raw)) {
			return { city: "", country: "" };
		}
		const segs = raw.split(",").map((s) => s.trim()).filter(Boolean);
		return {
			city: segs[0] || "",
			country: segs.length > 1 ? segs[segs.length - 1] : "",
		};
	}

	function parseKnowledgePeople(k) {
		if (!k) return [];
		const txt = String(k.text || k.content || "");
		const m = txt.match(/(?:^|\n)People:\s*([^\n]+)/i);
		if (!m) return [];
		return m[1]
			.split(",")
			.map((s) => s.trim().replace(/\.$/, ""))
			.filter((s) => s && !/^\d+$/.test(s));
	}

	const isCreationsDir = $derived(/\/(generated|creations|onmychat)(\/|$)/i.test(String(path || "")));

	const enrichedMedia = $derived.by(() => {
		const { byPath, byName } = knowledgeLookup;
		return mediaFiles.map((f) => {
			const fname = f.name || f.value || "";
			const idLow = String(f.id || "").toLowerCase();
			const matchedCard = byPath.get(idLow) || byName.get(fname.toLowerCase()) || null;

			const takenMs =
				(matchedCard?.taken_at ? parseExifOrIsoTimestamp(matchedCard.taken_at) : 0) ||
				extractFilenameTimestamp(fname) ||
				parseExifOrIsoTimestamp(f.date || f.lastModified || f.mtime) ||
				0;

			const cardText = matchedCard?.text || matchedCard?.content || "";
			const isCreation =
				isCreationsDir ||
				isLikelyGeneratedMedia({
					filename: fname,
					path: f.id || "",
					text: cardText,
					tags: matchedCard?.tags || [],
				});
			const parsedSidecar = isCreation && cardText ? parseReadmeSidecarText(cardText) : null;
			const loc = isCreation ? { city: "", country: "" } : parseKnowledgePlace(matchedCard);
			const people = parseKnowledgePeople(matchedCard);
			const isVid = isVideoFile(f);
			const isScreenshot = !isCreation && /^(screenshot|screen_|scr_)/i.test(fname);
			const modelInfo = isCreation
				? detectCreationModelInfo(fname, isVid, {
						modelHint: parsedSidecar?.model || "",
						prompt: parsedSidecar?.prompt || "",
						rawText: cardText,
				  })
				: null;

			const extracted = !isCreation && matchedCard
				? extractCardLandmarkAndEvent(cardText, matchedCard.title || "", loc.city)
				: { landmark: "", event: "" };
			const landmark = !isCreation ? String(matchedCard?.landmark || extracted.landmark || "").trim() : "";

			const extraPlaces = new Set();
			if (loc.city) {
				extraPlaces.add(loc.city.toLowerCase());
				extraPlaces.add(canonicalizePlaceKey(loc.city));
			}
			if (loc.country) extraPlaces.add(loc.country.toLowerCase());

			const tags = filterAndCanonicalizePhotoTags(matchedCard?.tags || [], {
				extraPlaceKeys: extraPlaces,
				peopleLower: new Set(people.map((p) => p.toLowerCase())),
				isCreation,
			}).filter((t) => !landmark || t !== landmark.toLowerCase());

			return {
				raw: f,
				id: f.id,
				name: fname,
				takenMs,
				isVideo: isVid,
				isCreation,
				isScreenshot,
				city: loc.city,
				country: loc.country,
				place: loc.city,
				landmark,
				people,
				modelInfo,
				tags,
			};
		});
	});

	const sortedEnriched = $derived.by(() => {
		const filtered = activeAlbum && activeAlbum.itemIds
			? enrichedMedia.filter((m) => activeAlbum.itemIds.has(m.id))
			: enrichedMedia;

		return [...filtered].sort((a, b) => {
			if (a.takenMs !== b.takenMs) {
				return crSort === "desc" ? b.takenMs - a.takenMs : a.takenMs - b.takenMs;
			}
			return crSort === "desc"
				? b.name.localeCompare(a.name, undefined, { numeric: true, sensitivity: "base" })
				: a.name.localeCompare(b.name, undefined, { numeric: true, sensitivity: "base" });
		});
	});

	const timelineGroups = $derived(
		buildTimelineDayGroups(sortedEnriched, {
			localeStr: $i18nLocale || undefined,
			undatedLabel: $i18nT("media_undated", {}, "Undated"),
		})
	);

	const monthScrubber = $derived(buildMonthScrubberMarkers(sortedEnriched, $i18nLocale || undefined));

	// Build Auto-Albums for current folder
	const crAutoAlbums = $derived.by(() => {
		const descAll = [...enrichedMedia].sort((a, b) => b.takenMs - a.takenMs);
		const isUnlocked = (m) => {
			const f = m.raw;
			const sharedLock = Boolean(
				f.isShared ||
				String(f.id || "").startsWith("/shared-to-me") ||
				String(f.parent || "").startsWith("/shared-to-me")
			);
			const sens = Boolean(f.isSensitive || hasSensitiveTag(f));
			return !sens || (!sharedLock && $contentModeStore === "fun");
		};
		const pickCover = (arr) => arr.find(isUnlocked)?.raw || arr[0]?.raw || null;

		// 1. People (if indexed)
		const pMap = new Map();
		for (const m of descAll) {
			for (const p of m.people) {
				const k = p.toLowerCase();
				if (!pMap.has(k)) pMap.set(k, { title: p, items: [] });
				pMap.get(k).items.push(m);
			}
		}
		const peopleAlbums = Array.from(pMap.entries())
			.map(([k, b]) => ({
				id: `person:${k}`,
				title: b.title,
				count: b.items.length,
				coverFile: pickCover(b.items),
				itemIds: new Set(b.items.map((x) => x.id)),
			}))
			.sort((a, b) => b.count - a.count);

		// 2. Places (if indexed)
		const plMap = new Map();
		for (const m of descAll) {
			if (!m.city) continue;
			const k = canonicalizePlaceKey(m.city);
			if (!plMap.has(k)) plMap.set(k, { title: m.city, subtitle: m.country, items: [] });
			plMap.get(k).items.push(m);
		}
		const placeAlbums = Array.from(plMap.entries())
			.map(([k, b]) => ({
				id: `place:${k}`,
				title: b.title,
				subtitle: b.subtitle,
				count: b.items.length,
				coverFile: pickCover(b.items),
				itemIds: new Set(b.items.map((x) => x.id)),
			}))
			.sort((a, b) => b.count - a.count);

		// 3. Moments / Date Clusters (strictly separated: real camera moments vs generated creative sessions)
		const locStr = $i18nLocale || undefined;
		const fmtSpan = (sDt, eDt) => {
			const sameDay =
				sDt.getFullYear() === eDt.getFullYear() &&
				sDt.getMonth() === eDt.getMonth() &&
				sDt.getDate() === eDt.getDate();
			return sameDay
				? sDt.toLocaleDateString(locStr, { day: "numeric", month: "short", year: "numeric" })
				: `${sDt.toLocaleDateString(locStr, { day: "numeric", month: "short" })} - ${eDt.toLocaleDateString(locStr, { day: "numeric", month: "short", year: "numeric" })}`;
		};

		const realAsc = descAll.filter((m) => !m.isCreation && m.takenMs > 0).slice().reverse();
		const realClusters = [];
		let curR = [];
		for (const m of realAsc) {
			if (!curR.length) {
				curR = [m];
			} else if ((m.takenMs - curR[curR.length - 1].takenMs) / 3600000 <= 36) {
				curR.push(m);
			} else {
				realClusters.push(curR);
				curR = [m];
			}
		}
		if (curR.length) realClusters.push(curR);

		const genAsc = descAll.filter((m) => m.isCreation && m.takenMs > 0).slice().reverse();
		const genClusters = [];
		let curG = [];
		for (const m of genAsc) {
			if (!curG.length) {
				curG = [m];
			} else {
				const prev = curG[curG.length - 1];
				const gapH = (m.takenMs - prev.takenMs) / 3600000;
				const sameMod = !m.modelInfo?.key || !prev.modelInfo?.key || m.modelInfo.key === prev.modelInfo.key;
				const spanH = (m.takenMs - curG[0].takenMs) / 3600000;
				if (gapH <= 12 && sameMod && spanH <= 72) {
					curG.push(m);
				} else {
					genClusters.push(curG);
					curG = [m];
				}
			}
		}
		if (curG.length) genClusters.push(curG);

		const realMomentAlbums = realClusters
			.filter((cl) => cl.length >= 3)
			.map((cl) => {
				const rev = [...cl].reverse();
				const dLabel = fmtSpan(new Date(cl[0].takenMs), new Date(cl[cl.length - 1].takenMs));
				const topPlace = rev.find((x) => x.city)?.city || "";
				return {
					id: `moment:real:${cl[0].takenMs}_${cl.length}`,
					title: topPlace ? `${topPlace} · ${dLabel}` : dLabel,
					endMs: cl[cl.length - 1].takenMs,
					count: rev.length,
					coverFile: pickCover(rev),
					itemIds: new Set(rev.map((x) => x.id)),
				};
			});

		const genMomentAlbums = genClusters
			.filter((cl) => cl.length >= 3)
			.map((cl) => {
				const rev = [...cl].reverse();
				const dLabel = fmtSpan(new Date(cl[0].takenMs), new Date(cl[cl.length - 1].takenMs));
				const topMod = rev.find((x) => x.modelInfo?.label)?.modelInfo?.label || "";
				return {
					id: `moment:gen:${cl[0].takenMs}_${cl.length}`,
					title: topMod ? `✨ ${topMod} · ${dLabel}` : `✨ ${dLabel}`,
					endMs: cl[cl.length - 1].takenMs,
					count: rev.length,
					coverFile: pickCover(rev),
					itemIds: new Set(rev.map((x) => x.id)),
				};
			});

		const momentAlbums = [...realMomentAlbums, ...genMomentAlbums]
			.sort((a, b) => b.endMs - a.endMs)
			.slice(0, 16);

		// 4. Media Types / Models / Tags
		const categoryAlbums = [];
		const vidItems = descAll.filter((m) => m.isVideo);
		if (vidItems.length > 0 && vidItems.length < descAll.length) {
			categoryAlbums.push({
				id: "type:videos",
				title: `🎬 ${$i18nT("media_videos", {}, "Videos")}`,
				count: vidItems.length,
				coverFile: pickCover(vidItems),
				itemIds: new Set(vidItems.map((x) => x.id)),
			});
		}
		const shotItems = descAll.filter((m) => m.isScreenshot);
		if (shotItems.length >= 2) {
			categoryAlbums.push({
				id: "type:screenshots",
				title: `📱 ${$i18nT("media_screenshots", {}, "Screenshots")}`,
				count: shotItems.length,
				coverFile: pickCover(shotItems),
				itemIds: new Set(shotItems.map((x) => x.id)),
			});
		}
		const lmMap = new Map();
		for (const m of descAll) {
			if (!m.landmark) continue;
			const k = m.landmark.toLowerCase();
			if (!lmMap.has(k)) lmMap.set(k, { title: m.landmark, items: [] });
			lmMap.get(k).items.push(m);
		}
		for (const [k, b] of lmMap.entries()) {
			categoryAlbums.push({
				id: `landmark:${k}`,
				title: `🏛️ ${b.title}`,
				count: b.items.length,
				coverFile: pickCover(b.items),
				itemIds: new Set(b.items.map((x) => x.id)),
			});
		}

		const modMap = new Map();
		for (const m of descAll) {
			if (!m.modelInfo) continue;
			if (!modMap.has(m.modelInfo.key)) modMap.set(m.modelInfo.key, { title: m.modelInfo.label, items: [] });
			modMap.get(m.modelInfo.key).items.push(m);
		}
		for (const [k, b] of modMap.entries()) {
			categoryAlbums.push({
				id: `model:${k}`,
				title: `🎛️ ${b.title}`,
				count: b.items.length,
				coverFile: pickCover(b.items),
				itemIds: new Set(b.items.map((x) => x.id)),
			});
		}

		const tMap = new Map();
		for (const m of descAll) {
			for (const t of m.tags) {
				if (!tMap.has(t)) tMap.set(t, []);
				tMap.get(t).push(m);
			}
		}
		for (const [t, arr] of Array.from(tMap.entries()).filter(([, a]) => a.length >= 2).sort((a, b) => b[1].length - a[1].length).slice(0, 18)) {
			categoryAlbums.push({
				id: `tag:${t}`,
				title: `#${t}`,
				count: arr.length,
				coverFile: pickCover(arr),
				itemIds: new Set(arr.map((x) => x.id)),
			});
		}

		const keepSubgroup = (alb) => !(hasStoryComic && subfolders.length === 0 && alb.count >= descAll.length);
		const filtPeople = peopleAlbums.filter(keepSubgroup);
		const filtPlaces = placeAlbums.filter(keepSubgroup);
		const filtMoments = momentAlbums.filter(keepSubgroup);
		const filtCategories = categoryAlbums.filter(keepSubgroup);

		const total = subfolders.length + filtPeople.length + filtPlaces.length + filtMoments.length + filtCategories.length;
		return {
			people: filtPeople,
			places: filtPlaces,
			moments: filtMoments,
			categories: filtCategories,
			totalCount: total,
		};
	});

	const hasGroupingAlbums = $derived(crAutoAlbums.totalCount > 0 || subfolders.length > 0);
	const effectiveCrTab = $derived(hasGroupingAlbums ? crTab : "timeline");
	const selectedMediaEntries = $derived(
		enrichedMedia.filter((m) => Array.isArray(selected) && selected.includes(m.id))
	);
	const showTopbar = $derived(
		hasGroupingAlbums ||
		Boolean(activeAlbum) ||
		selectedMediaEntries.length > 0 ||
		timelineGroups.length > 1
	);

	function triggerShareStory(entries, title = "") {
		if (!entries || !entries.length || typeof window === "undefined") return;
		window.dispatchEvent(
			new CustomEvent("omd-open-share-story", {
				detail: {
					items: entries,
					albumTitle: title || "",
					explicitParentFolderId: path && path !== "/" && !path.startsWith("/shared-") ? path : "",
					isCreations: isCreationsDir,
				},
			})
		);
	}

	function triggerShareStoryForAlbum(alb, e) {
		if (e) e.stopPropagation();
		if (!alb || !alb.itemIds) return;
		const entries = enrichedMedia.filter((m) => alb.itemIds.has(m.id));
		triggerShareStory(entries, alb.title || "");
	}

	function handleComicImageClick(payload) {
		const targetLow = String(payload?.filename || "").toLowerCase();
		if (!targetLow) return;
		const matched = (files || []).find(
			(f) => String(f?.name || f?.value || "").toLowerCase() === targetLow
		);
		if (matched) {
			if (typeof window !== "undefined") {
				window.__omdCameraRollScopeNames = new Set(
					mediaFiles.map((x) => x.name || x.value || "").filter(Boolean)
				);
			}
			api.exec("open-file", { id: matched.id });
		}
	}

	function handleShareCurrentFolder() {
		if (!path || path === "/" || typeof window === "undefined") return;
		window.dispatchEvent(
			new CustomEvent("omd-share-folder", {
				detail: { folderId: path },
			})
		);
	}

	function handlePublishCurrentStoryToCivitai() {
		if (!path || path === "/" || typeof window === "undefined") return;
		window.dispatchEvent(
			new CustomEvent("omd-publish-story-civitai", {
				detail: { folderId: path },
			})
		);
	}

	function getCoverPreview(fileObj) {
		if (!fileObj || !templates?.preview) return "";
		if ((fileObj.type === "folder" || fileObj.isDir) && !fileObj.hasCover && !fileObj.customAttributes?.cover) {
			return "";
		}
		return templates.preview(fileObj, 320, 240) || "";
	}

	function getFolderIcon(folder) {
		if (!folder || !templates?.icon) return "";
		return templates.icon(folder, "big") || "";
	}

	function isCoverBlurred(fileObj) {
		if (!fileObj) return false;
		const sharedLock = Boolean(
			fileObj.isShared ||
			String(fileObj.id || "").startsWith("/shared-to-me") ||
			String(fileObj.parent || "").startsWith("/shared-to-me")
		);
		const sens = Boolean(fileObj.isSensitive || hasSensitiveTag(fileObj));
		return Boolean(sens && (sharedLock || $contentModeStore !== "fun"));
	}

	function jumpToCrMonth(sc) {
		if (!sc) return;
		activeScrubberKey = sc.key;
		mobileScrubberOpen = false;
		if (scrollEl) {
			const el = scrollEl.querySelector(`[data-cr-month="${sc.key}"]`);
			if (el) {
				el.scrollIntoView({ behavior: "smooth", block: "start" });
			}
		}
	}

	function handleCrScroll() {
		if (!scrollEl || monthScrubber.length < 2) return;
		const anchors = scrollEl.querySelectorAll("[data-cr-month]");
		const topRef = scrollEl.getBoundingClientRect().top;
		let cur = monthScrubber[0]?.key || "";
		for (const el of anchors) {
			if (el.getBoundingClientRect().top - topRef <= 110) {
				cur = el.getAttribute("data-cr-month") || cur;
			} else {
				break;
			}
		}
		activeScrubberKey = cur;
	}

	function findScopeForItemId(id) {
		if (activeAlbum) {
			return sortedEnriched.map((m) => m.raw);
		}
		for (const g of timelineGroups) {
			if (g.items.some((m) => m.id === id)) {
				return g.items.map((m) => m.raw);
			}
		}
		return null;
	}

	function click(id, e) {
		if (id === "/wx-filemanager-parent-link") {
			return backToParent();
		}
		if (!id || id === "body") return;

		const target = e && e.target;

		const checkTarget = target && target.closest(".wx-cr-check");
		if (checkTarget) {
			api.exec("select-file", {
				id,
				toggle: true,
				panel: $activePanel,
			});
			return;
		}

		const moreTarget =
			target &&
			(target.closest(".wx-more") !== null ||
				(typeof target.className === "string" &&
					(target.className.indexOf("wx-more") !== -1 ||
						target.className.indexOf("wxi-dots-v") !== -1)));
		if (moreTarget) {
			return;
		}

		if (e && (e.ctrlKey || e.metaKey || e.shiftKey)) {
			const ctrl = e.ctrlKey || e.metaKey;
			const shift = e.shiftKey;
			api.exec("select-file", {
				id,
				toggle: ctrl,
				range: shift,
				panel: $activePanel,
			});
			return;
		}

		if ($mode === "search") {
			api.exec("filter-files", {
				text: "",
			});
		}

		const item = files ? files.find((a) => a.id === id) : null;
		if (item) {
			if (item.type === "folder" || item.isDir) {
				api.exec("set-path", {
					id: item.id,
					panel: $activePanel,
				});
			} else {
				const scopeRaw = findScopeForItemId(item.id);
				if (typeof window !== "undefined" && Array.isArray(scopeRaw) && scopeRaw.length > 0) {
					window.__omdCameraRollScopeNames = new Set(scopeRaw.map((x) => x.name || x.value || "").filter(Boolean));
				}
				api.exec("open-file", {
					id: item.id,
				});
			}
		}
	}

	function backToParent() {
		if (crumbs && crumbs.length > 1) {
			api.exec("set-path", {
				id: crumbs[crumbs.length - 2].id,
				panel: $activePanel,
				selected: [crumbs[crumbs.length - 1].id],
			});
		} else if (path && path !== "/") {
			const parentPath = path.substring(0, path.lastIndexOf("/")) || "/";
			api.exec("set-path", {
				id: parentPath,
				panel: $activePanel,
			});
		}
	}

	function openFolder(folderId) {
		if (!folderId) return;
		api.exec("set-path", {
			id: folderId,
			panel: $activePanel,
		});
	}

	function dblclick() {}

	function applySelection(id, ev) {
		if (!selected?.length || !selected.some((i) => i?.id === id)) {
			click(id, ev);
		}
	}
</script>

<svelte:window bind:innerWidth />

{#if $mode === "search" && !mediaFiles.length && !subfolders.length}
	<div class="wx-not-found">
		<div class="wx-not-found-text">{_("Looks like nothing is here")}</div>
	</div>
{:else}
	<div class="wx-cr-shell">
		{#if (mediaFiles.length > 0 || subfolders.length > 0) && showTopbar}
			<div class="wx-cr-topbar">
				<div class="wx-cr-topbar-left">
					{#if hasGroupingAlbums}
						<div class="wx-cr-seg" role="tablist">
							<button
								type="button"
								role="tab"
								class="wx-cr-seg-btn"
								class:active={effectiveCrTab === "timeline" && !activeAlbum}
								onclick={() => setCrTab("timeline")}
							>
								<span>{$i18nT("media_timeline", {}, "Timeline")}</span>
								<span class="wx-cr-pill-cnt">{mediaFiles.length}</span>
							</button>
							<button
								type="button"
								role="tab"
								class="wx-cr-seg-btn"
								class:active={effectiveCrTab === "albums" || Boolean(activeAlbum)}
								onclick={() => setCrTab("albums")}
							>
								<span>{$i18nT("media_auto_albums", {}, "Auto-Albums")}</span>
								{#if crAutoAlbums.totalCount > 0}
									<span class="wx-cr-pill-cnt">{crAutoAlbums.totalCount}</span>
								{/if}
							</button>
						</div>
					{/if}

					{#if activeAlbum}
						<div class="wx-cr-crumb">
							<button type="button" class="wx-cr-back-btn" onclick={closeAlbum}>
								← {$i18nT("media_all_albums", {}, "All albums")}
							</button>
							<span class="wx-cr-crumb-title">{activeAlbum.title} ({sortedEnriched.length})</span>
							{#if !path?.startsWith("/shared-to-me")}
								<button
									type="button"
									class="wx-cr-story-cta"
									onclick={() => triggerShareStory(sortedEnriched, activeAlbum.title)}
								>
									📖 {$i18nT("share_a_story", {}, "Share a story")}
								</button>
							{/if}
						</div>
					{/if}
				</div>

				<div class="wx-cr-topbar-right">
					{#if selectedMediaEntries.length > 0 && !path?.startsWith("/shared-to-me")}
						<button
							type="button"
							class="wx-cr-story-cta"
							onclick={() => triggerShareStory(selectedMediaEntries, activeAlbum?.title || "")}
						>
							📖 {$i18nT("share_a_story", {}, "Share a story")} ({selectedMediaEntries.length})
						</button>
					{/if}

					{#if sortedEnriched.length > 1}
						<button
							type="button"
							class="wx-cr-icon-btn"
							class:active={crSort === "asc"}
							title={crSort === "desc" ? $i18nT("sort_date_desc", {}, "Date (newest first)") : $i18nT("sort_date_asc", {}, "Date (oldest first)")}
							onclick={toggleCrSort}
						>
							<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
								{#if crSort === "desc"}
									<path d="M4 6h9M4 12h7M4 18h5" />
									<path d="M16 15l3 3 3-3" />
									<path d="M19 6v12" />
								{:else}
									<path d="M4 6h5M4 12h7M4 18h9" />
									<path d="M16 9l3-3 3 3" />
									<path d="M19 18V6" />
								{/if}
							</svg>
						</button>
					{/if}

					{#if isMobile && (effectiveCrTab === "timeline" || activeAlbum) && monthScrubber.length >= 2}
						<button
							type="button"
							class="wx-cr-icon-btn"
							class:active={mobileScrubberOpen}
							title={$i18nT("media_jump_date", {}, "Jump to date")}
							onclick={() => (mobileScrubberOpen = !mobileScrubberOpen)}
						>
							<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
								<rect x="3" y="4" width="18" height="18" rx="2" ry="2" />
								<line x1="16" y1="2" x2="16" y2="6" />
								<line x1="8" y1="2" x2="8" y2="6" />
								<line x1="3" y1="10" x2="21" y2="10" />
							</svg>
						</button>
					{/if}
				</div>
			</div>
		{/if}

		<!-- svelte-ignore a11y_no_noninteractive_tabindex -->
		<div
			tabindex="0"
			class="wx-cameraroll-container"
			bind:this={scrollEl}
			onscroll={handleCrScroll}
			data-id={setID("body")}
			use:delegateClick={{ click, dblclick, context: applySelection }}
		>
			{#if effectiveCrTab === "albums" && !activeAlbum}
				<!-- CAMERAROLL AUTO-ALBUMS VIEW -->
				<div class="wx-cr-albums-wrap">
					{#if subfolders.length > 0}
						<div class="wx-cr-album-sec">
							<div class="wx-cr-sec-head">
								<h4>
									<svg class="wx-cr-head-folder-svg" viewBox="0 0 16 16" width="16" height="16" aria-hidden="true">
										<path fill="currentColor" d="M9.828 3h3.982a2 2 0 0 1 1.992 2.181l-.637 7A2 2 0 0 1 13.174 14H2.825a2 2 0 0 1-1.991-1.819l-.637-7a2 2 0 0 1 .342-1.31L.5 3a2 2 0 0 1 2-2h3.672a2 2 0 0 1 1.414.586l.828.828A2 2 0 0 0 9.828 3m-8.322.12q.322-.119.684-.12h5.396l-.707-.707A1 1 0 0 0 6.172 2H2.5a1 1 0 0 0-1 .981z" />
									</svg>
									<span>{$i18nT("media_folders", {}, "Folders")}</span>
								</h4>
								<span class="wx-cr-sec-cnt">{subfolders.length}</span>
							</div>
							<div class="wx-cr-albums-grid">
								{#each subfolders as folder (folder.id)}
									{@const fCov = getCoverPreview(folder)}
									{@const showCov = Boolean(fCov && !failedFolderCovers[folder.id])}
									{@const fIcon = getFolderIcon(folder)}
									{@const isSpecialFolder = folder.id === "/shared-to-me" || folder.id === "/shared-by-me" || folder.name === "shared-to-me" || folder.name === "shared-by-me" || folder.isRootSection}
									<div
										role="button"
										tabindex="0"
										data-id={setID(folder.id)}
										class="wx-cr-album-card wx-cr-folder-card"
										class:wx-selected={Array.isArray(selected) && selected.indexOf(folder.id) >= 0}
										onkeydown={(e) => {
											if (e.key === "Enter" || e.key === " ") {
												e.preventDefault();
												openFolder(folder.id);
											}
										}}
									>
										<div class="wx-cr-album-cover" class:no-cover={!showCov}>
											{#if showCov}
												<img
													src={fCov}
													alt={folder.name || folder.value}
													loading="lazy"
													onerror={() => {
														failedFolderCovers = { ...failedFolderCovers, [folder.id]: true };
													}}
												/>
											{:else}
												<div class="wx-cr-album-fallback folder-fallback">
													{#if fIcon}
														<img class="wx-cr-folder-icon" src={fIcon} alt="" />
													{:else}
														<svg class="wx-cr-folder-svg" viewBox="0 0 16 16" aria-hidden="true">
															<path fill="currentColor" d="M9.828 3h3.982a2 2 0 0 1 1.992 2.181l-.637 7A2 2 0 0 1 13.174 14H2.825a2 2 0 0 1-1.991-1.819l-.637-7a2 2 0 0 1 .342-1.31L.5 3a2 2 0 0 1 2-2h3.672a2 2 0 0 1 1.414.586l.828.828A2 2 0 0 0 9.828 3m-8.322.12q.322-.119.684-.12h5.396l-.707-.707A1 1 0 0 0 6.172 2H2.5a1 1 0 0 0-1 .981z" />
														</svg>
													{/if}
												</div>
											{/if}
											{#if !isSpecialFolder}
												<div
													data-action-id={setID(folder.id)}
													class="wx-more wx-cr-folder-more"
													title={_("Actions")}
												>
													<i class="wxi-dots-v"></i>
												</div>
											{/if}
											<div class="wx-cr-album-grad">
												<span class="wx-cr-album-title">{folder.name || folder.value}</span>
											</div>
										</div>
									</div>
								{/each}
							</div>
						</div>
					{/if}

					{#if crAutoAlbums.people.length > 0}
						<div class="wx-cr-album-sec">
							<div class="wx-cr-sec-head">
								<h4>👥 {$i18nT("media_people", {}, "People")}</h4>
								<span class="wx-cr-sec-cnt">{crAutoAlbums.people.length}</span>
							</div>
							<div class="wx-cr-albums-grid compact">
								{#each crAutoAlbums.people as pAlb (pAlb.id)}
									{@const cov = getCoverPreview(pAlb.coverFile)}
									<div class="wx-cr-album-card-wrap">
										<button type="button" class="wx-cr-album-card compact" onclick={(e) => { e.stopPropagation(); openAlbum(pAlb); }}>
											<div class="wx-cr-album-cover">
												{#if cov}
													<img src={cov} alt={pAlb.title} loading="lazy" class:wx-cr-cov-blur={isCoverBlurred(pAlb.coverFile)} />
												{:else}
													<div class="wx-cr-album-fallback">👤</div>
												{/if}
												<div class="wx-cr-album-grad">
													<span class="wx-cr-album-title">{pAlb.title}</span>
												</div>
												<span class="wx-cr-album-cnt">{pAlb.count}</span>
											</div>
										</button>
										{#if !path?.startsWith("/shared-to-me")}
											<button
												type="button"
												class="wx-cr-album-story-btn"
												title={$i18nT("share_a_story", {}, "Share a story")}
												onclick={(e) => triggerShareStoryForAlbum(pAlb, e)}
											>📖</button>
										{/if}
									</div>
								{/each}
							</div>
						</div>
					{/if}

					{#if crAutoAlbums.moments.length > 0}
						<div class="wx-cr-album-sec">
							<div class="wx-cr-sec-head">
								<h4>🗓️ {$i18nT("media_moments", {}, "Moments & Trips")}</h4>
								<span class="wx-cr-sec-cnt">{crAutoAlbums.moments.length}</span>
							</div>
							<div class="wx-cr-albums-grid">
								{#each crAutoAlbums.moments as mAlb (mAlb.id)}
									{@const cov = getCoverPreview(mAlb.coverFile)}
									<div class="wx-cr-album-card-wrap">
										<button type="button" class="wx-cr-album-card" onclick={(e) => { e.stopPropagation(); openAlbum(mAlb); }}>
											<div class="wx-cr-album-cover">
												{#if cov}
													<img src={cov} alt={mAlb.title} loading="lazy" class:wx-cr-cov-blur={isCoverBlurred(mAlb.coverFile)} />
												{:else}
													<div class="wx-cr-album-fallback">🗓️</div>
												{/if}
												<div class="wx-cr-album-grad">
													<span class="wx-cr-album-title">{mAlb.title}</span>
												</div>
												<span class="wx-cr-album-cnt">{mAlb.count}</span>
											</div>
										</button>
										{#if !path?.startsWith("/shared-to-me")}
											<button
												type="button"
												class="wx-cr-album-story-btn"
												title={$i18nT("share_a_story", {}, "Share a story")}
												onclick={(e) => triggerShareStoryForAlbum(mAlb, e)}
											>📖</button>
										{/if}
									</div>
								{/each}
							</div>
						</div>
					{/if}

					{#if crAutoAlbums.places.length > 0}
						<div class="wx-cr-album-sec">
							<div class="wx-cr-sec-head">
								<h4>📍 {$i18nT("media_places", {}, "Places")}</h4>
								<span class="wx-cr-sec-cnt">{crAutoAlbums.places.length}</span>
							</div>
							<div class="wx-cr-albums-grid">
								{#each crAutoAlbums.places as plAlb (plAlb.id)}
									{@const cov = getCoverPreview(plAlb.coverFile)}
									<div class="wx-cr-album-card-wrap">
										<button type="button" class="wx-cr-album-card" onclick={(e) => { e.stopPropagation(); openAlbum(plAlb); }}>
											<div class="wx-cr-album-cover">
												{#if cov}
													<img src={cov} alt={plAlb.title} loading="lazy" class:wx-cr-cov-blur={isCoverBlurred(plAlb.coverFile)} />
												{:else}
													<div class="wx-cr-album-fallback">📍</div>
												{/if}
												<div class="wx-cr-album-grad">
													<span class="wx-cr-album-title">{plAlb.title}</span>
												</div>
												<span class="wx-cr-album-cnt">{plAlb.count}</span>
											</div>
										</button>
										{#if !path?.startsWith("/shared-to-me")}
											<button
												type="button"
												class="wx-cr-album-story-btn"
												title={$i18nT("share_a_story", {}, "Share a story")}
												onclick={(e) => triggerShareStoryForAlbum(plAlb, e)}
											>📖</button>
										{/if}
									</div>
								{/each}
							</div>
						</div>
					{/if}

					{#if crAutoAlbums.categories.length > 0}
						<div class="wx-cr-album-sec">
							<div class="wx-cr-sec-head">
								<h4>🏷️ {$i18nT("media_themes", {}, "Themes & Tags")}</h4>
								<span class="wx-cr-sec-cnt">{crAutoAlbums.categories.length}</span>
							</div>
							<div class="wx-cr-albums-grid compact">
								{#each crAutoAlbums.categories as cAlb (cAlb.id)}
									{@const cov = getCoverPreview(cAlb.coverFile)}
									<div class="wx-cr-album-card-wrap">
										<button type="button" class="wx-cr-album-card compact" onclick={(e) => { e.stopPropagation(); openAlbum(cAlb); }}>
											<div class="wx-cr-album-cover">
												{#if cov}
													<img src={cov} alt={cAlb.title} loading="lazy" class:wx-cr-cov-blur={isCoverBlurred(cAlb.coverFile)} />
												{:else}
													<div class="wx-cr-album-fallback">#</div>
												{/if}
												<div class="wx-cr-album-grad">
													<span class="wx-cr-album-title">{cAlb.title}</span>
												</div>
												<span class="wx-cr-album-cnt">{cAlb.count}</span>
											</div>
										</button>
										{#if !path?.startsWith("/shared-to-me")}
											<button
												type="button"
												class="wx-cr-album-story-btn"
												title={$i18nT("share_a_story", {}, "Share a story")}
												onclick={(e) => triggerShareStoryForAlbum(cAlb, e)}
											>📖</button>
										{/if}
									</div>
								{/each}
							</div>
						</div>
					{/if}

					{#if crAutoAlbums.totalCount === 0}
						<div class="wx-not-found">
							<div class="wx-not-found-text">{$i18nT("media_no_albums_yet", {}, "Switch to Timeline to browse all media.")}</div>
						</div>
					{/if}
				</div>
			{:else}
				<!-- CHRONOLOGICAL TIMELINE STREAM (+ COMIC STORY ZONE AT TOP) -->
				{#if hasStoryComic && !activeAlbum}
					<div class="wx-cr-story-lead">
						<StoryComicView
							markdown={folderReadme}
							folderTitle={path?.split("/").filter(Boolean).pop() || ""}
							isDarkTheme={isDark}
							mediaVersion={localMediaVer}
							resolveMediaUrl={(rel) =>
								resolveFolderMediaAssetUrl(path, files, rel, {
									templates,
									onAsyncReady: () => (localMediaVer += 1),
								})}
							resolvePosterUrl={(rel) =>
								resolveFolderMediaAssetUrl(path, files, rel, {
									forPoster: true,
									templates,
								})}
							isSensitiveMedia={(rel) => {
								const fname = String(rel || "").split("/").pop().toLowerCase();
								const f = (files || []).find(
									(x) => String(x?.name || x?.value || "").toLowerCase() === fname
								);
								return Boolean(f && (f.isSensitive || hasSensitiveTag(f)));
							}}
							canShare={Boolean(path && path !== "/" && !path.startsWith("/shared-to-me"))}
							canPublishCivitai={Boolean(isCreationsDir && path && path.split("/").filter(Boolean).length >= 3)}
							onImageClick={handleComicImageClick}
							onShare={handleShareCurrentFolder}
							onPublishCivitai={handlePublishCurrentStoryToCivitai}
						/>
					</div>
				{/if}

				{#if subfolders.length && !activeAlbum}
					<div class="wx-cr-folders-row">
						{#each subfolders as folder (folder.id)}
							<button
								type="button"
								class="wx-cr-folder-chip"
								data-id={setID(folder.id)}
								title={folder.name || folder.value}
								onclick={(e) => {
									e.stopPropagation();
									openFolder(folder.id);
								}}
							>
								<i class="wxi-folder"></i>
								<span class="wx-cr-folder-name">{folder.name || folder.value}</span>
							</button>
						{/each}
					</div>
				{/if}

				{#if timelineGroups.length}
					<div class="wx-cr-timeline-stream">
						{#each timelineGroups as group, gIdx (group.dayKey)}
							{@const isNewMonth = gIdx === 0 || timelineGroups[gIdx - 1].monthKey !== group.monthKey}
							<section class="wx-cr-day-group" data-cr-month={isNewMonth ? group.monthKey : undefined}>
								{#if hasStoryComic && !activeAlbum && timelineGroups.length === 1}
									<div class="wx-cr-story-footer-head">
										<span>📎 {$i18nT("story_files_footer", {}, "Files")} · {group.items.length}</span>
									</div>
								{:else}
									<div class="wx-cr-day-head">
										<div class="wx-cr-day-title-wrap">
											<span class="wx-cr-day-label">{group.dayLabel}</span>
											{#if group.topPlace}
												<span class="wx-cr-day-place">📍 {group.topPlace}</span>
											{/if}
										</div>
										<span class="wx-cr-day-cnt">{group.items.length}</span>
									</div>
								{/if}
								<div class="wx-cameraroll-grid">
									{#each group.items as entry (entry.id)}
										<Item item={entry.raw} />
									{/each}
								</div>
							</section>
						{/each}
					</div>
				{/if}
			{/if}
		</div>

		{#if (effectiveCrTab === "timeline" || activeAlbum) && monthScrubber.length >= 2 && (!isMobile || mobileScrubberOpen)}
			{#if isMobile && mobileScrubberOpen}
				<!-- svelte-ignore a11y_click_events_have_key_events -->
				<!-- svelte-ignore a11y_no_static_element_interactions -->
				<div class="wx-cr-scrubber-backdrop" onclick={() => (mobileScrubberOpen = false)}></div>
			{/if}
			<nav class="wx-cr-date-scrubber" class:mobile-open={isMobile && mobileScrubberOpen} aria-label="Folder timeline jump bar">
				{#each monthScrubber.slice(0, 36) as sc (sc.key)}
					<button
						type="button"
						class="wx-cr-scrubber-tick"
						class:active={activeScrubberKey === sc.key}
						title={sc.fullLabel}
						onclick={() => jumpToCrMonth(sc)}
					>
						<span>{sc.shortLabel} '{String(sc.year).slice(-2)}</span>
						<span class="wx-cr-tick-dot"></span>
					</button>
				{/each}
			</nav>
		{/if}
	</div>
{/if}

<style>
	.wx-cr-shell {
		position: relative;
		display: flex;
		flex-direction: column;
		height: 100%;
		width: 100%;
		overflow: hidden;
	}

	.wx-cr-topbar {
		display: flex;
		align-items: center;
		justify-content: space-between;
		gap: 8px;
		padding: 8px 12px;
		border-bottom: 1px solid var(--wx-border, rgba(255, 255, 255, 0.1));
		flex-shrink: 0;
		flex-wrap: wrap;
	}

	.wx-cr-topbar-left,
	.wx-cr-topbar-right {
		display: flex;
		align-items: center;
		gap: 8px;
		flex-wrap: wrap;
	}

	.wx-cr-seg {
		display: inline-flex;
		align-items: center;
		gap: 2px;
		padding: 3px;
		border-radius: 999px;
		background: var(--wx-background-alt, rgba(125, 125, 125, 0.12));
		border: 1px solid var(--wx-border, rgba(255, 255, 255, 0.1));
	}

	.wx-cr-seg-btn {
		display: inline-flex;
		align-items: center;
		gap: 5px;
		padding: 4px 11px;
		border-radius: 999px;
		border: none;
		background: transparent;
		color: var(--wx-color-font, inherit);
		opacity: 0.74;
		font-size: 12.5px;
		font-weight: 600;
		cursor: pointer;
		transition: all 0.15s ease;
	}

	.wx-cr-seg-btn.active {
		opacity: 1;
		background: var(--wx-color-primary, #7c3aed);
		color: #ffffff;
	}

	.wx-cr-pill-cnt {
		font-size: 11px;
		padding: 1px 6px;
		border-radius: 999px;
		background: rgba(255, 255, 255, 0.2);
	}

	.wx-cr-crumb {
		display: inline-flex;
		align-items: center;
		gap: 8px;
	}

	.wx-cr-back-btn {
		padding: 4px 10px;
		border-radius: 8px;
		border: 1px solid var(--wx-border, rgba(255, 255, 255, 0.15));
		background: var(--wx-background-alt, rgba(125, 125, 125, 0.12));
		color: var(--wx-color-font, inherit);
		font-size: 12.5px;
		font-weight: 600;
		cursor: pointer;
	}

	.wx-cr-crumb-title {
		font-size: 13px;
		font-weight: 700;
		color: var(--wx-color-font, inherit);
	}

	.wx-cr-icon-btn {
		width: 30px;
		height: 30px;
		border-radius: 8px;
		border: 1px solid var(--wx-border, rgba(255, 255, 255, 0.14));
		background: var(--wx-background-alt, rgba(125, 125, 125, 0.1));
		color: var(--wx-color-font, inherit);
		display: inline-flex;
		align-items: center;
		justify-content: center;
		cursor: pointer;
	}

	.wx-cr-icon-btn.active {
		color: #ec4899;
		border-color: rgba(236, 72, 153, 0.45);
		background: rgba(236, 72, 153, 0.12);
	}

	.wx-cameraroll-container {
		flex-grow: 1;
		height: 100%;
		overflow-y: auto;
		display: flex;
		flex-direction: column;
		outline: none;
		box-sizing: border-box;
		-webkit-overflow-scrolling: touch;
		cursor: default;
		padding-bottom: 32px;
	}

	.wx-cr-folders-row {
		display: flex;
		flex-wrap: wrap;
		gap: 8px;
		padding: 10px 10px 6px;
		flex-shrink: 0;
		background: transparent;
		cursor: default;
	}

	.wx-cr-folder-chip {
		display: inline-flex;
		align-items: center;
		gap: 6px;
		padding: 6px 12px;
		border-radius: 8px;
		background: var(--wx-background, rgba(255, 255, 255, 0.06));
		border: 1px solid var(--wx-border, rgba(255, 255, 255, 0.12));
		color: var(--wx-color-font, #ECE7F6);
		font-size: 13px;
		font-weight: 500;
		cursor: pointer;
		white-space: nowrap;
		user-select: none;
		transition: background-color 0.15s ease, border-color 0.15s ease, transform 0.1s ease;
	}

	.wx-cr-folder-chip:hover {
		background: var(--wx-background-hover, rgba(255, 255, 255, 0.12));
		border-color: var(--wx-color-primary, #929CD4);
		transform: translateY(-1px);
	}

	.wx-cr-folder-chip i {
		font-size: 16px;
		color: var(--wx-color-primary, #5863A6) !important;
	}

	.wx-cr-timeline-stream {
		display: flex;
		flex-direction: column;
		gap: 16px;
		padding-right: 34px;
	}

	.wx-cr-day-group {
		display: flex;
		flex-direction: column;
		gap: 4px;
	}

	.wx-cr-day-head {
		position: sticky;
		top: 0;
		z-index: 6;
		display: flex;
		align-items: center;
		justify-content: space-between;
		padding: 7px 12px 5px;
		background: color-mix(in srgb, var(--wx-background, #14161f) 88%, transparent);
		backdrop-filter: blur(8px);
	}

	.wx-cr-day-title-wrap {
		display: flex;
		align-items: center;
		gap: 8px;
		flex-wrap: wrap;
	}

	.wx-cr-day-label {
		font-size: 13.5px;
		font-weight: 700;
		color: var(--wx-color-font, inherit);
		text-transform: capitalize;
	}

	.wx-cr-day-place {
		font-size: 11.5px;
		padding: 1px 8px;
		border-radius: 999px;
		background: var(--wx-background-alt, rgba(125, 125, 125, 0.15));
		color: var(--wx-color-font-alt, inherit);
	}

	.wx-cr-day-cnt {
		font-size: 12px;
		color: var(--wx-color-font-alt, inherit);
		opacity: 0.75;
	}

	.wx-cameraroll-grid {
		display: grid;
		grid-template-columns: repeat(auto-fill, minmax(175px, 1fr));
		gap: 4px;
		padding: 2px 8px;
		align-content: flex-start;
		box-sizing: border-box;
		width: 100%;
		cursor: default;
	}

	/* Auto-Albums Grid inside File Manager */
	.wx-cr-albums-wrap {
		display: flex;
		flex-direction: column;
		gap: 24px;
		padding: 14px 14px 36px;
	}

	.wx-cr-album-sec {
		display: flex;
		flex-direction: column;
		gap: 10px;
	}

	.wx-cr-sec-head {
		display: flex;
		align-items: center;
		gap: 8px;
	}

	.wx-cr-sec-head h4 {
		margin: 0;
		font-size: 14.5px;
		font-weight: 700;
		color: var(--wx-color-font, inherit);
		display: inline-flex;
		align-items: center;
		gap: 7px;
	}

	.wx-cr-head-folder-svg {
		color: var(--wx-color-primary, #929CD4);
		flex-shrink: 0;
	}

	.wx-cr-sec-cnt {
		font-size: 11.5px;
		padding: 1px 7px;
		border-radius: 999px;
		background: var(--wx-background-alt, rgba(125, 125, 125, 0.15));
		color: var(--wx-color-font-alt, inherit);
	}

	.wx-cr-albums-grid {
		display: grid;
		grid-template-columns: repeat(auto-fill, minmax(200px, 1fr));
		gap: 12px;
	}

	.wx-cr-albums-grid.compact {
		grid-template-columns: repeat(auto-fill, minmax(160px, 1fr));
	}

	.wx-cr-album-card {
		padding: 0;
		border-radius: 12px;
		overflow: hidden;
		border: 1px solid var(--wx-border, rgba(255, 255, 255, 0.12));
		background: var(--wx-background-alt, rgba(125, 125, 125, 0.1));
		cursor: pointer;
		text-align: left;
		transition: transform 0.15s ease, border-color 0.15s ease;
	}

	.wx-cr-album-card:hover {
		transform: translateY(-2px);
		border-color: var(--wx-color-primary, #7c3aed);
	}

	.wx-cr-album-cover {
		position: relative;
		height: 138px;
		overflow: hidden;
		background: rgba(0, 0, 0, 0.12);
	}

	.wx-cr-album-card.compact .wx-cr-album-cover {
		height: 112px;
	}

	.wx-cr-album-cover > img {
		width: 100%;
		height: 100%;
		object-fit: cover;
		display: block;
	}

	.wx-cr-cov-blur {
		filter: blur(22px) saturate(0.7);
		transform: scale(1.12);
	}

	.wx-cr-album-fallback {
		width: 100%;
		height: 100%;
		display: flex;
		align-items: center;
		justify-content: center;
		font-size: 28px;
		box-sizing: border-box;
	}

	.wx-cr-album-fallback.folder-fallback {
		padding-bottom: 14px;
		color: var(--wx-color-primary, #929CD4);
	}

	.wx-cr-album-fallback .wx-cr-folder-icon,
	.wx-cr-album-fallback .wx-cr-folder-svg {
		width: 66px;
		height: 66px;
		object-fit: contain;
		display: block;
		color: var(--wx-color-primary, #929CD4);
	}

	:global(.light-theme) .wx-cr-album-cover.no-cover {
		background: var(--wx-background, #ffffff);
	}

	:global(.light-theme) .wx-cr-album-cover.no-cover .wx-cr-album-grad {
		background: linear-gradient(to top, rgba(242, 244, 248, 0.96), transparent);
		color: var(--wx-color-font, #1e293b);
	}

	.wx-cr-folder-card.wx-selected {
		border-color: var(--wx-color-primary, #7c3aed);
		box-shadow: 0 0 0 1px var(--wx-color-primary, #7c3aed);
	}

	.wx-cr-folder-more {
		position: absolute;
		top: 7px;
		right: 7px;
		width: 26px;
		height: 26px;
		border-radius: 50%;
		background: rgba(0, 0, 0, 0.48);
		backdrop-filter: blur(4px);
		color: #ffffff;
		display: flex;
		align-items: center;
		justify-content: center;
		opacity: 0;
		transform: scale(0.88);
		transition: opacity 0.15s ease, transform 0.15s ease, background 0.15s ease;
		z-index: 3;
		font-size: 15px;
		cursor: pointer;
	}

	.wx-cr-folder-card:hover .wx-cr-folder-more,
	.wx-cr-folder-card:focus-within .wx-cr-folder-more {
		opacity: 1;
		transform: scale(1);
	}

	.wx-cr-folder-more:hover {
		background: rgba(0, 0, 0, 0.78);
	}

	:global(.light-theme) .wx-cr-album-cover.no-cover .wx-cr-folder-more {
		background: rgba(15, 23, 42, 0.08);
		color: var(--wx-color-font, #334155);
	}

	:global(.light-theme) .wx-cr-album-cover.no-cover .wx-cr-folder-more:hover {
		background: rgba(15, 23, 42, 0.16);
	}

	@media (max-width: 768px) {
		.wx-cr-folder-more {
			opacity: 1;
			transform: scale(1);
		}
	}

	.wx-cr-album-grad {
		position: absolute;
		inset: auto 0 0 0;
		padding: 20px 10px 8px;
		background: linear-gradient(to top, rgba(0, 0, 0, 0.78), transparent);
		color: #ffffff;
	}

	.wx-cr-album-title {
		font-size: 13px;
		font-weight: 700;
	}

	.wx-cr-album-cnt {
		position: absolute;
		top: 7px;
		right: 7px;
		padding: 2px 7px;
		border-radius: 999px;
		background: rgba(0, 0, 0, 0.58);
		color: #ffffff;
		font-size: 11px;
		font-weight: 600;
	}

	/* Scrubber */
	.wx-cr-scrubber-backdrop {
		position: absolute;
		inset: 0;
		z-index: 16;
		background: rgba(0, 0, 0, 0.16);
	}

	.wx-cr-date-scrubber {
		position: absolute;
		right: 5px;
		top: 52%;
		transform: translateY(-50%);
		z-index: 18;
		display: flex;
		flex-direction: column;
		align-items: flex-end;
		gap: 3px;
		padding: 7px 4px;
		border-radius: 11px;
		background: color-mix(in srgb, var(--wx-background, #181b26) 86%, transparent);
		border: 1px solid var(--wx-border, rgba(255, 255, 255, 0.12));
		backdrop-filter: blur(6px);
		max-height: 76%;
		overflow-y: auto;
	}

	.wx-cr-date-scrubber.mobile-open {
		padding: 10px 8px;
		gap: 6px;
		box-shadow: 0 10px 26px rgba(0, 0, 0, 0.35);
	}

	.wx-cr-scrubber-tick {
		display: inline-flex;
		align-items: center;
		gap: 5px;
		background: transparent;
		border: none;
		color: var(--wx-color-font-alt, inherit);
		font-size: 11px;
		padding: 2px 5px;
		border-radius: 6px;
		cursor: pointer;
		white-space: nowrap;
	}

	.wx-cr-date-scrubber.mobile-open .wx-cr-scrubber-tick {
		font-size: 12.5px;
		padding: 5px 8px;
	}

	.wx-cr-scrubber-tick:hover,
	.wx-cr-scrubber-tick.active {
		color: var(--wx-color-primary, #ec4899);
		font-weight: 700;
	}

	.wx-cr-tick-dot {
		width: 5px;
		height: 5px;
		border-radius: 50%;
		background: currentColor;
	}

	@media (max-width: 768px) {
		.wx-cr-timeline-stream {
			padding-right: 0;
		}
	}

	@media (max-width: 600px) {
		.wx-cameraroll-grid {
			grid-template-columns: repeat(3, 1fr);
			gap: 2px;
			padding: 2px;
		}
	}

	@media (min-width: 601px) and (max-width: 960px) {
		.wx-cameraroll-grid {
			grid-template-columns: repeat(auto-fill, minmax(130px, 1fr));
			gap: 3px;
			padding: 3px;
		}
	}

	.wx-not-found {
		display: flex;
		flex-direction: column;
		justify-content: center;
		align-items: center;
		gap: 5px;
		height: 100%;
	}

	.wx-not-found-text {
		text-align: center;
		color: var(--wx-color-font-alt);
	}

	.wx-cr-story-lead {
		padding: 6px 14px 0;
	}

	.wx-cr-story-footer-head {
		display: flex;
		align-items: center;
		padding: 6px 12px 8px;
		font-size: 12.5px;
		font-weight: 650;
		opacity: 0.72;
		border-top: 1px solid var(--wx-border, rgba(128, 128, 150, 0.16));
		margin: 4px 8px 2px;
	}

	.wx-cr-story-cta {
		appearance: none;
		border: 1px solid rgba(139, 92, 246, 0.45);
		background: linear-gradient(135deg, rgba(124, 58, 237, 0.2), rgba(236, 72, 153, 0.18));
		color: inherit;
		border-radius: 999px;
		padding: 5px 12px;
		font-size: 12px;
		font-weight: 650;
		cursor: pointer;
		display: inline-flex;
		align-items: center;
		gap: 5px;
		transition: transform 0.12s ease, background 0.15s ease;
	}

	.wx-cr-story-cta:hover {
		transform: translateY(-1px);
		background: linear-gradient(135deg, rgba(124, 58, 237, 0.34), rgba(236, 72, 153, 0.28));
	}

	.wx-cr-album-card-wrap {
		position: relative;
		display: flex;
		width: 100%;
	}

	.wx-cr-album-card-wrap > .wx-cr-album-card {
		width: 100%;
	}

	.wx-cr-album-story-btn {
		position: absolute;
		top: 8px;
		left: 8px;
		z-index: 4;
		width: 28px;
		height: 28px;
		border-radius: 50%;
		border: 1px solid rgba(255, 255, 255, 0.25);
		background: rgba(15, 15, 25, 0.78);
		color: #fff;
		font-size: 13px;
		display: flex;
		align-items: center;
		justify-content: center;
		cursor: pointer;
		opacity: 0;
		transform: scale(0.92);
		transition: opacity 0.15s ease, transform 0.15s ease, background 0.15s ease;
	}

	.wx-cr-album-card-wrap:hover .wx-cr-album-story-btn,
	.wx-cr-album-story-btn:focus-visible {
		opacity: 1;
		transform: scale(1);
	}

	.wx-cr-album-story-btn:hover {
		background: rgba(124, 58, 237, 0.92);
	}

	@media (hover: none) {
		.wx-cr-album-story-btn {
			opacity: 0.92;
			transform: scale(1);
		}
	}
</style>
