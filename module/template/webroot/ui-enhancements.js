(function () {
  const themeKey = "inline-hook-spoof-theme";
  const root = document.documentElement;
  const themeButton = document.getElementById("theme-toggle");
  const searchBox = document.getElementById("app-search-box");
  const searchButton = document.getElementById("search-toggle");
  const searchInput = document.getElementById("search-input");
  const appList = document.getElementById("app-list");
  const runningList = document.getElementById("running-app-list");
  const runningSection = document.getElementById("running-apps-section");
  const loading = document.getElementById("loading");
  const emptySearch = document.getElementById("empty-search");
  const appCount = document.getElementById("app-count");
  const batchSize = 60;
  let requestId = 0;
  let renderId = 0;
  let batchObserver;
  let iconObserver;
  let filterTimer;

  function applyTheme(theme) {
    root.dataset.theme = theme;
    const dark = theme === "dark";
    themeButton.textContent = dark ? "☀️" : "🌙";
    themeButton.setAttribute("aria-label", dark ? "استخدام النسق النهاري" : "استخدام النسق الليلي");
    themeButton.title = themeButton.getAttribute("aria-label");
  }

  try {
    const saved = localStorage.getItem(themeKey);
    applyTheme(saved === "dark" || saved === "light" ? saved :
      (window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light"));
  } catch (error) {
    console.warn("Theme storage unavailable:", error);
    applyTheme("light");
  }

  window.toggleTheme = function () {
    const theme = root.dataset.theme === "dark" ? "light" : "dark";
    applyTheme(theme);
    try {
      localStorage.setItem(themeKey, theme);
    } catch (error) {
      console.warn("Could not persist theme:", error);
    }
  };

  window.toggleSearch = function () {
    const opening = searchBox.hidden;
    searchBox.hidden = !opening;
    searchButton.hidden = opening;
    if (opening) {
      if (!document.getElementById("home").classList.contains("active")) {
        openTab("home", document.getElementById("nav-home"));
      }
      searchInput.focus();
    } else {
      searchInput.value = "";
      filterApps();
      searchButton.focus();
    }
  };

  searchInput.addEventListener("keydown", event => {
    if (event.key === "Escape") window.toggleSearch();
  });

  createAppCard = function (app) {
    const card = document.createElement("div");
    card.className = "card" + (selectedPackages.has(app.pkg) ? " selected" : "");
    const icon = document.createElement("div");
    icon.className = "icon";
    const image = document.createElement("img");
    image.hidden = true;
    image.loading = "lazy";
    image.decoding = "async";
    image.dataset.src = "ksu://icon/" + encodeURIComponent(app.pkg);
    image.onerror = function () {
      if (iconObserver) iconObserver.unobserve(icon);
      const fallback = document.createElement("span");
      fallback.className = "material-icons-round";
      fallback.textContent = "android";
      image.replaceWith(fallback);
    };
    icon.appendChild(image);
    const info = document.createElement("div");
    info.className = "info";
    const name = document.createElement("div");
    name.className = "name";
    name.textContent = app.name;
    const pkg = document.createElement("div");
    pkg.className = "pkg";
    pkg.textContent = app.pkg;
    info.append(name, pkg);
    const check = document.createElement("span");
    check.className = "material-icons-round check";
    check.textContent = "check_circle";
    card.append(icon, info, check);
    card.addEventListener("mousedown", event => startLongClick(event, app.pkg));
    card.addEventListener("touchstart", event => startLongClick(event, app.pkg), { passive: true });
    ["mouseup", "mouseleave", "touchend", "touchcancel", "touchmove"].forEach(type =>
      card.addEventListener(type, endLongClick));
    card.addEventListener("click", event => {
      if (event.target.closest("#context-menu")) return;
      if (selectedPackages.has(app.pkg)) {
        selectedPackages.delete(app.pkg);
      } else {
        selectedPackages.add(app.pkg);
      }
      card.classList.toggle("selected", selectedPackages.has(app.pkg));
      saveConfig();
    });
    return card;
  };

  function observeIcons(fragment) {
    fragment.querySelectorAll(".icon").forEach(container => {
      const image = container.querySelector("img[data-src]");
      if (!image) return;
      if (iconObserver) {
        iconObserver.observe(container);
      } else {
        image.hidden = false;
        image.src = image.dataset.src;
      }
    });
  }

  function appendCards(target, apps) {
    const fragment = document.createDocumentFragment();
    apps.forEach(app => fragment.appendChild(createAppCard(app)));
    observeIcons(fragment);
    if (target === appList && target.querySelector("[data-batch-end]")) {
      target.insertBefore(fragment, target.querySelector("[data-batch-end]"));
    } else {
      target.appendChild(fragment);
    }
  }

  renderAppList = function (apps) {
    renderId++;
    const current = renderId;
    if (batchObserver) batchObserver.disconnect();
    if (iconObserver) iconObserver.disconnect();
    batchObserver = null;
    iconObserver = typeof IntersectionObserver === "function"
      ? new IntersectionObserver(entries => {
        entries.forEach(entry => {
          const image = entry.target.querySelector("img[data-src]");
          if (!image) return;
          if (entry.isIntersecting) {
            if (!image.hasAttribute("src")) image.src = image.dataset.src;
            image.hidden = false;
          } else if (image.hasAttribute("src")) {
            image.removeAttribute("src");
            image.hidden = true;
          }
        });
      }, { rootMargin: "180px" }) : null;

    appList.replaceChildren();
    runningList.replaceChildren();
    emptySearch.hidden = apps.length !== 0 || !searchInput.value.trim();
    const running = apps.filter(app => app.isRunning);
    runningSection.style.display = running.length ? "block" : "none";
    appendCards(runningList, running);
    appCount.textContent = `${apps.length} apps`;

    let offset = 0;
    const sentinel = document.createElement("div");
    sentinel.dataset.batchEnd = "";
    const nextBatch = () => {
      if (current !== renderId || offset >= apps.length) return;
      const end = Math.min(offset + batchSize, apps.length);
      appendCards(appList, apps.slice(offset, end));
      offset = end;
      if (offset >= apps.length && batchObserver) batchObserver.disconnect();
    };
    nextBatch();
    if (offset < apps.length) {
      appList.appendChild(sentinel);
      if (typeof IntersectionObserver === "function") {
        batchObserver = new IntersectionObserver(entries => {
          if (entries[0].isIntersecting && current === renderId) nextBatch();
        }, { rootMargin: "500px" });
        batchObserver.observe(sentinel);
      } else {
        const onScroll = () => {
          if (current !== renderId) {
            window.removeEventListener("scroll", onScroll);
          } else if (sentinel.getBoundingClientRect().top < window.innerHeight + 500) {
            nextBatch();
            if (offset >= apps.length) window.removeEventListener("scroll", onScroll);
          }
        };
        window.addEventListener("scroll", onScroll, { passive: true });
        onScroll();
      }
    }
  };

  filterApps = function () {
    clearTimeout(filterTimer);
    filterTimer = setTimeout(() => {
      const query = searchInput.value.trim().toLocaleLowerCase();
      renderAppList(query ? allApps.filter(app =>
        app.name.toLocaleLowerCase().includes(query) ||
        app.pkg.toLocaleLowerCase().includes(query)) : allApps);
    }, 80);
  };

  loadApps = async function () {
    const current = ++requestId;
    loading.style.display = "block";
    loading.textContent = "جارٍ تحميل التطبيقات…";
    const runningPromise = getRunningPackages();
    const output = await exec("pm list packages -3");
    if (current !== requestId) return;
    if (!output.trim() && !window.ksu) {
      loading.textContent = "تعذّر عرض التطبيقات: افتح الصفحة من مدير الوحدة.";
      return;
    }
    const packages = output.split("\n").map(line =>
      line.replace(/^package:/, "").trim()).filter(Boolean).sort();
    allApps = packages.map(pkg => ({ pkg, name: pkg, isRunning: false }));
    loading.style.display = "none";
    if (!packages.length) {
      loading.textContent = "لم يُعثر على تطبيقات مستخدم مثبّتة.";
      loading.style.display = "block";
    }
    const query = searchInput.value.trim().toLocaleLowerCase();
    renderAppList(query ? allApps.filter(app => app.pkg.toLocaleLowerCase().includes(query)) : allApps);

    await new Promise(resolve => setTimeout(resolve, 0));
    if (current !== requestId) return;
    const running = await runningPromise;
    if (current !== requestId) return;
    allApps.forEach(app => { app.isRunning = running.has(app.pkg); });
    filterApps();
    const info = await getPackagesInfo(packages);
    if (current !== requestId) return;
    const labels = new Map(info.map(app => [app.packageName, app.appLabel]));
    allApps.forEach(app => { app.name = labels.get(app.pkg) || app.pkg; });
    filterApps();
  };
})();
