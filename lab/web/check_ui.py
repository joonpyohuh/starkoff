"""Optional browser integration check. Requires Playwright in a separate test environment.
Start `scripts/run_lab.sh --qemu --web --quiet` first, then run this file with that Python.
The check changes the simulated charger state and runs TC_055_CS.
"""

from pathlib import Path
from playwright.sync_api import sync_playwright, expect


def main():
    out = Path(__file__).resolve().parents[1] / "results"
    out.mkdir(exist_ok=True)
    errors = []
    with sync_playwright() as p:
        browser = p.chromium.launch(headless=True, args=["--no-sandbox"])
        page = browser.new_page(viewport={"width": 1440, "height": 1000})
        page.on("pageerror", lambda error: errors.append(str(error)))
        page.goto("http://127.0.0.1:8080", wait_until="networkidle")
        expect(page.locator("#s-status")).to_have_text("사용 가능", timeout=30000)
        page.locator('[data-tag="TAG001"]').focus()
        page.keyboard.press("Enter")
        expect(page.locator("#s-status")).to_have_text("준비 중", timeout=15000)
        page.locator('[data-cmd="plug 1"]').click()
        expect(page.locator("#s-status")).to_have_text("충전 중", timeout=15000)
        expect(page.locator("#metric-count")).not_to_have_text("0")
        page.screenshot(path=str(out / "web-desktop.png"), full_page=True)
        page.locator("#remote-stop").click()
        expect(page.locator("#s-status")).to_have_text("충전 종료", timeout=15000)
        page.locator('[data-cmd="unplug 1"]').click()
        expect(page.locator("#s-status")).to_have_text("사용 가능", timeout=15000)
        page.locator("#reserve").click()
        expect(page.locator("#s-status")).to_have_text("예약됨", timeout=15000)
        page.locator("#cancel-reserve").click()
        expect(page.locator("#s-status")).to_have_text("사용 가능", timeout=15000)
        page.locator("#trigger-meter").click()
        expect(page.locator("#remote-reply")).to_contain_text("TriggerMessage:", timeout=15000)
        expect(page.locator("#log")).to_contain_text("MeterValues", timeout=15000)
        page.locator("#list-version").click()
        expect(page.locator("#remote-reply")).to_contain_text("listVersion", timeout=15000)
        page.evaluate('ws.send(JSON.stringify({t:"cmd",cmd:"plug 1\\nreboot"}))')
        expect(page.locator("#remote-reply")).to_contain_text("ERR not allowed", timeout=5000)
        page.locator("button.tbtn").filter(has_text="TC_055").click()
        expect(page.locator("button.tbtn.PASS").filter(has_text="TC_055")).to_be_visible(timeout=30000)
        page.reload(wait_until="networkidle")
        expect(page.locator("button.tbtn.PASS").filter(has_text="TC_055")).to_be_visible()
        page.set_viewport_size({"width": 390, "height": 844})
        page.screenshot(path=str(out / "web-mobile.png"), full_page=True)
        assert page.evaluate("document.documentElement.scrollWidth <= innerWidth"), "horizontal overflow"
        assert not errors, errors
        print(
            "PASS: keyboard card -> charging -> remote stop; reservation/cancel; trigger; SQLite timing; command rejection; test result/reload; mobile width; no JS errors"
        )
        browser.close()


if __name__ == "__main__":
    main()
