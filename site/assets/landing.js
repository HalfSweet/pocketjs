// Homepage glue: the framework code tabs. Everything else is CSS.
for (const group of document.querySelectorAll("[data-subtabs]")) {
  const tabs = group.querySelectorAll(".subtab");
  tabs.forEach(t => t.addEventListener("click", () => {
    tabs.forEach(x => x.classList.remove("on"));
    group.querySelectorAll("pre").forEach(p => p.classList.remove("on"));
    t.classList.add("on");
    group.querySelector("#" + t.dataset.sub).classList.add("on");
  }));
}

// The scale panel reveals its markers once, when it scrolls into view.
const scale = document.querySelector("[data-scale]");
if (scale) {
  if (!("IntersectionObserver" in window)) scale.classList.add("on");
  else {
    const io = new IntersectionObserver((entries) => {
      for (const e of entries) {
        if (!e.isIntersecting) continue;
        e.target.classList.add("on");
        io.unobserve(e.target);
      }
    }, { threshold: 0.35 });
    io.observe(scale);
  }
}
