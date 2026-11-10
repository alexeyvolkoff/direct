<script>
	import { setContext } from "svelte";

	import Layout from "./Layout.svelte";

	import { Locale } from "@svar-ui/svelte-core";
	import { en } from "@svar-ui/filemanager-locales";

	// stores
	import { EventBusRouter } from "@svar-ui/lib-state";
	import { DataStore, getMenuOptions } from "../store/index.js";
	import Modals from "./Modals.svelte";
	import { whitelist } from "../icons";

	let {
		data = [],
		mode = "cards",
		drive = null,
		preview = false,
		panels = [],
		activePanel = 0,
		readonly = false,
		menuOptions = getMenuOptions,
		extraInfo = null,
		init = null,
		icons = function (file, size) {
			const { type, ext } = file;

			if (type === "folder") return false;

			let icon;

			if (type && type !== "file" && whitelist[type]) {
				icon = type;
			} else if (ext) {
				icon = whitelist[ext] ? ext : "file";
			} else icon = "unknown";

			return `https://cdn.svar.dev/icons/filemanager/vivid/${size}/${icon}.svg`;
		},
		previews = null,
		httpDrive = null,
		searchDrivesProvider = null,
		toolbarActions = null,
		...restProps
	} = $props();

	// init stores
	const dataStore = new DataStore();

	// define event route
	let firstInRoute = dataStore.in;

	const dash = /-/g;
	let lastInRoute = new EventBusRouter((a, b) => {
		const name = "on" + a.replace(dash, "");
		if (restProps[name]) {
			restProps[name](b);
		}
	});
	firstInRoute.setNext(lastInRoute);

	// public API
	export const // state
		getState = dataStore.getState.bind(dataStore),
		getReactiveState = dataStore.getReactive.bind(dataStore),
		getStores = () => ({ data: dataStore }),
		// events
		exec = firstInRoute.exec,
		setNext = ev => (lastInRoute = lastInRoute.setNext(ev)),
		intercept = firstInRoute.intercept.bind(firstInRoute),
		on = firstInRoute.on.bind(firstInRoute),
		detach = firstInRoute.detach.bind(firstInRoute),
		//specific
		getFile = id => dataStore.getFile(id),
		serialize = id => dataStore.serialize(id);

	const api = {
		getState,
		getReactiveState,
		getStores,
		exec,
		setNext,
		intercept,
		on,
		detach,
		getFile,
		serialize,
	};

	import { writable } from "svelte/store";
	const searchViewMode = writable("cards");

	firstInRoute.on("filter-files", () => {
		try {
			const cur = dataStore.getState().mode;
			if (cur && cur !== "search") {
				searchViewMode.set(cur === "table" ? "table" : "cards");
			}
		} catch (_) {}
	});

	firstInRoute.on("set-search-mode", ({ mode }) => {
		searchViewMode.set(mode === "table" ? "table" : "cards");
	});

	const none = () => null;
	// common API available in components
	setContext("filemanager-store", {
		getReactiveState: () => {
			const state = dataStore.getReactive();
			return {
				...state,
				searchViewMode,
			};
		},
		exec: firstInRoute.exec.bind(firstInRoute),
		templates: {
			preview: previews || none,
			icon: icons == "simple" ? none : icons,
		},
		getFile: dataStore.getFile.bind(dataStore),
		getHttpDrive: () => httpDrive,
	});
	setContext("filemanager-http-drive", () => httpDrive);

	let init_once = true;
	const reinitStore = () => {
		if (init_once) {
			dataStore.init({
				data,
				mode,
				drive,
				preview,
				panels,
				activePanel,
			});

			if (init) {
				init(api);
			}
			init_once = false;
			return;
		}

		dataStore.setState({ drive });
	};
	reinitStore();
	$effect(reinitStore);
</script>

<Locale words={en} optional={true}>
	<Modals>
		<Layout {readonly} {menuOptions} {extraInfo} {httpDrive} {searchDrivesProvider} {toolbarActions} />
	</Modals>
</Locale>
