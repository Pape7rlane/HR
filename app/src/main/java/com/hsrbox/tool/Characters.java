package com.hsrbox.tool;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.*;

/**
 * Playable characters: AvatarId and Chinese name (124 up to 7.1 and six of 7.2, tests/avatar_ids.txt), the internal model name
 * (skeleton root Avatar_Girl_Catalyst_Citlali → citlali) and the names mods use for them, for search and for the
 * recommended character of an imported mod.
 */
public final class Characters {
    private Characters() {}

    public static final class Avatar {
        public final int id; public final String name;
        /** Internal model names, as in the game's skeleton root; written to match.txt for an import without a bone library. */
        public final List<String> model;
        /** Lower-case names a mod may use (internal and English names, joined forms such as hutao, raidenshogun). */
        public final List<String> aliases;
        Avatar(int id, String name, String model, String extra) {
            this.id = id; this.name = name; this.model = split(model);
            List<String> all = new ArrayList<>(this.model); for (String s : split(extra)) if (!all.contains(s)) all.add(s); aliases = Collections.unmodifiableList(all);
        }
        @Override public String toString() { return name + "  " + id; }
    }
    private static List<String> split(String s) { List<String> out = new ArrayList<>(); for (String w : s.trim().split("\\s+")) if (!w.isEmpty()) out.add(w); return Collections.unmodifiableList(out); }
    private static Avatar a(int id, String name, String model, String extra) { return new Avatar(id, name, model, extra); }

    /**
     * 崩铁的可玩角色：4 位 AvatarID 与中文名（内部模型名、mod 里常见的写法）。ID 取自游戏数据
     * （AvatarConfigExcel 的 AvatarID），目前收录到 1.x 主力名单；缺的往里加，或直接丢一个
     * characters.json 到 HSRBox 目录（格式见 external()），不用重编。
     * 模型名用于在 mod 名字里猜角色，也写进 match.txt。
     */
    private static final Avatar[] BUILTIN = {
        a(1001, "三月七", "march7th", "march7"),
        a(1002, "丹恒", "danheng", ""),
        a(1003, "姬子", "himeko", ""),
        a(1004, "瓦尔特", "welt", "weltjoyce"),
        a(1005, "卡芙卡", "kafka", ""),
        a(1006, "银狼", "silverwolf", "silver wolf"),
        a(1007, "阿兰", "arlan", ""),
        a(1008, "艾丝妲", "asta", ""),
        a(1009, "黑塔", "herta", "theherta"),
        a(1101, "布洛妮娅", "bronya", "bronya rand"),
        a(1102, "希儿", "seele", ""),
        a(1103, "希露瓦", "serval", ""),
        a(1104, "杰帕德", "gepard", ""),
        a(1105, "娜塔莎", "natasha", ""),
        a(1106, "佩拉", "pela", "pelageya sergeyevna"),
        a(1107, "克拉拉", "clara", ""),
        a(1108, "桑博", "sampo", "sampo koski"),
        a(1109, "虎克", "hook", ""),
        a(1110, "玲可", "lynx", "landau lynx"),
        a(1111, "卢卡", "luka", ""),
        a(1112, "托帕", "topaz", "topaz numby"),
        a(1201, "青雀", "qingque", "qing que"),
        a(1202, "停云", "tingyun", "ting yun"),
        a(1203, "罗刹", "luocha", ""),
        a(1204, "景元", "jingyuan", "jing yuan"),
        a(1205, "刃", "blade", "ren"),
        a(1206, "素裳", "sushang", "su shang"),
        a(1207, "驭空", "yukong", "yu kong"),
        a(1208, "符玄", "fuxuan", "fu xuan"),
        a(1209, "彦卿", "yanqing", "yan qing"),
        a(1210, "桂乃芬", "guinaifen", "guinai fen"),
        a(1211, "白露", "bailu", "bai lu"),
        a(1212, "镜流", "jingliu", "jing liu"),
        a(1213, "丹恒·饮月", "danhengil", "imbibitor lunae dh il"),
        a(1214, "雪衣", "xueyi", "xue yi"),
        a(1215, "寒鸦", "hanya", "han ya"),
        a(1217, "藿藿", "huohuo", "huo huo"),
        a(8001, "开拓者（毁灭）", "trailblazer", "playerboy playergirl trailblazer"),
        a(8002, "开拓者（存护）", "trailblazerfire", "trailblazer fire"),
        a(8003, "开拓者（同谐）", "trailblazerharmony", "trailblazer harmony")
    };

    private static volatile List<Avatar> table;

    /** 内置表 + characters.json 里的补充（同 ID 时以 json 为准）。 */
    private static List<Avatar> rows() {
        List<Avatar> t = table;
        if (t != null) return t;
        synchronized (Characters.class) {
            t = table; if (t != null) return t;
            List<Avatar> out = new ArrayList<>(Arrays.asList(BUILTIN));
            for (Avatar x : external()) {
                int i = -1; for (int k = 0; k < out.size(); k++) if (out.get(k).id == x.id) { i = k; break; }
                if (i >= 0) out.set(i, x); else out.add(x);
            }
            t = table = Collections.unmodifiableList(out);
            return t;
        }
    }

    /** HSRBox 目录下的 characters.json：数组，每项 {"id":1001,"name":"三月七","model":"march7th","aliases":"march7"}。
     *  三个服各自 media 目录都看一遍；文件缺、写坏都不报错，只用内置表。 */
    private static List<Avatar> external() {
        List<Avatar> out = new ArrayList<>();
        String[] pkgs = {"com.miHoYo.hkrpg", "com.miHoYo.hkrpg.bilibili", "com.HoYoverse.hkrpgoversea"};
        for (String pkg : pkgs) {
            File f = new File("/storage/emulated/0/Android/media/" + pkg + "/HSRBox/characters.json");
            if (!f.isFile()) continue;
            try {
                Object root = Json.parse(new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8));
                if (!(root instanceof List)) continue;
                for (Object o : (List<?>) root) {
                    if (!(o instanceof Map)) continue;
                    Map<?, ?> m = (Map<?, ?>) o;
                    Object id = m.get("id"); if (!(id instanceof Number)) continue;
                    int n = ((Number) id).intValue(); if (n < 1000 || n > 9999) continue;
                    String name = m.get("name") instanceof String ? (String) m.get("name") : Integer.toString(n);
                    String model = m.get("model") instanceof String ? (String) m.get("model") : "";
                    String alias = m.get("aliases") instanceof String ? (String) m.get("aliases") : "";
                    out.add(a(n, name, model, alias));
                }
            } catch (Exception ignored) { }
        }
        return out;
    }
    public static List<Avatar> all() { return rows(); }
    /** Not a character (1.0.3): a texture-only mod for NPCs and monsters, whose meshes the game side finds anywhere in the
     *  scene by the index-buffer hashes its rules name (texture_diag.hpp kScene). Listed last in the picker. */
    public static final int SCENE = 1;
    public static final Avatar SCENE_AVATAR = new Avatar(SCENE, "NPC / 怪物 / 风之翼", "", "npc monster monsters enemy enemies glider flycloak 怪物 敌人 风之翼 滑翔");
    public static boolean isScene(int id) { return id == SCENE; }
    public static Avatar byId(int id) { if (id == SCENE) return SCENE_AVATAR; for (Avatar a : rows()) if (a.id == id) return a; return null; }
    public static String nameOf(int id) { Avatar a = byId(id); return a != null ? a.name : id == 0 ? "未指定角色" : "角色 " + id; }

    /** By ID or by name (Chinese, English or internal), in list order. An empty query lists everyone. */
    public static List<Avatar> search(String query) {
        String q = query == null ? "" : query.trim().toLowerCase(Locale.ROOT);
        if (q.isEmpty()) { List<Avatar> out = new ArrayList<>(all()); out.add(SCENE_AVATAR); return out; }
        String compact = q.replaceAll("[^a-z0-9]", ""); List<Avatar> out = new ArrayList<>();
        for (Avatar a : rows()) {
            boolean hit = Integer.toString(a.id).contains(q) || a.name.toLowerCase(Locale.ROOT).contains(q);
            if (!hit && compact.length() >= 2 && !compact.matches("[0-9]+")) for (String s : a.aliases) if (s.contains(compact)) { hit = true; break; }
            if (hit) out.add(a);
        }
        boolean scene = SCENE_AVATAR.name.toLowerCase(Locale.ROOT).contains(q); for (String s : SCENE_AVATAR.aliases) scene = scene || s.contains(q);
        if (scene) out.add(SCENE_AVATAR);
        return out;
    }

    /** Characters a mod most likely belongs to (several only when they score the same), best first, and the words seen. */
    public static final class Guess {
        public final List<Avatar> recommended = new ArrayList<>(); public final Map<Integer, Integer> scores = new LinkedHashMap<>(); public String evidence = "";
        public boolean recommends(int id) { for (Avatar a : recommended) if (a.id == id) return true; return false; }
    }
    /** Words of a name: split at anything not a letter or digit, at lower→upper case changes and at letter/digit changes. */
    static List<String> words(String s) {
        List<String> out = new ArrayList<>(); StringBuilder cur = new StringBuilder(); char prev = 0;
        for (int i = 0; i <= s.length(); i++) {
            char c = i < s.length() ? s.charAt(i) : ' '; boolean alnum = c < 128 && Character.isLetterOrDigit(c);
            boolean split = !alnum || (Character.isUpperCase(c) && Character.isLowerCase(prev)) || (cur.length() > 0 && Character.isDigit(c) != Character.isDigit(prev));
            if (split && cur.length() > 0) { out.add(cur.toString().toLowerCase(Locale.ROOT)); cur.setLength(0); }
            if (alnum) cur.append(c);
            prev = alnum ? c : 0;
        }
        return out;
    }
    /** Every word plus every two or three adjacent words joined (Hu Tao → hutao, RaidenShogun → raidenshogun). */
    static void count(String phrase, int weight, Map<String, Integer> counts) {
        List<String> w = words(phrase);
        for (int i = 0; i < w.size(); i++) { StringBuilder k = new StringBuilder(); for (int j = i; j < w.size() && j < i + 3; j++) { k.append(w.get(j)); counts.merge(k.toString(), weight, Integer::sum); } }
    }
    /**
     * Scores every character by the names in the mod: the name it was imported under and its folder and INI names count
     * three times, every INI section ([TextureOverrideCitlaliBody]) and resource file name (CitlaliHeadDiffuse.dds) once.
     * Characters in the bone library also match by the model name the game itself gave them.
     */
    public static Guess guess(File modRoot, String importedAs, List<ModLibrary.Character> library) throws Exception { return guess(modRoot, ModLibrary.ini(modRoot), importedAs, library); }
    public static Guess guess(File modRoot, File ini, String importedAs, List<ModLibrary.Character> library) throws Exception {
        Map<String, Integer> counts = new HashMap<>();
        count(importedAs == null ? "" : importedAs, 3, counts); count(modRoot.getName(), 3, counts); count(ini.getName().replaceFirst("(?i)\\.ini$", ""), 3, counts);
        for (String l : new String(Files.readAllBytes(ini.toPath()), StandardCharsets.UTF_8).split("\\r?\\n")) {
            String t = l.trim();
            if (t.startsWith("[") && t.endsWith("]")) count(t.substring(1, t.length() - 1), 1, counts);
            else if (t.toLowerCase(Locale.ROOT).startsWith("filename")) { int eq = t.indexOf('='); if (eq > 0) count(new File(t.substring(eq + 1).trim().replace('\\', '/').replace("\"", "")).getName().replaceFirst("\\.[A-Za-z0-9]+$", ""), 1, counts); }
        }
        Map<Integer, Set<String>> recorded = new HashMap<>();
        if (library != null) for (ModLibrary.Character c : library) for (String seg : c.root.split("/")) if (seg.toLowerCase(Locale.ROOT).startsWith("avatar")) recorded.computeIfAbsent(c.id, k -> new LinkedHashSet<>()).addAll(ModLibrary.tokens(seg));
        Guess g = new Guess(); int best = 0; StringBuilder why = new StringBuilder();
        for (Avatar a : rows()) {
            Set<String> names = new LinkedHashSet<>(a.aliases); Set<String> own = recorded.get(a.id); if (own != null) names.addAll(own);
            int score = 0; String hit = null; for (String n : names) { Integer c = counts.get(n); if (c != null) { score += c; if (hit == null) hit = n; } }
            if (score > 0) { g.scores.put(a.id, score); if (why.length() < 200) why.append(why.length() == 0 ? "" : ", ").append(hit).append('=').append(score); }
            best = Math.max(best, score);
        }
        if (best > 0) for (Avatar a : rows()) if (g.scores.getOrDefault(a.id, 0) == best) g.recommended.add(a);
        g.evidence = why.toString(); return g;
    }
    /** Adds what the mod's hashes say (DeviceHashes.characters: character -> its hashes), ten per hash: a hash this
     *  device or the PC database ties to one character outweighs any name. The recommendation is made again. */
    public static void hashEvidence(Guess g, Map<Integer, List<String>> hits) {
        if (hits.isEmpty()) return;
        StringBuilder why = new StringBuilder(g.evidence);
        for (Map.Entry<Integer, List<String>> e : hits.entrySet()) {
            int add = 10 * e.getValue().size(); g.scores.merge(e.getKey(), add, Integer::sum);
            if (why.length() < 300) why.append(why.length() == 0 ? "" : ", ").append("hash ").append(e.getValue().get(0)).append(e.getValue().size() > 1 ? " 等 " + e.getValue().size() + " 个" : "").append('=').append(add);
        }
        int best = 0; for (int v : g.scores.values()) best = Math.max(best, v);
        g.recommended.clear(); if (best > 0) for (Avatar a : rows()) if (g.scores.getOrDefault(a.id, 0) == best) g.recommended.add(a);
        if (best > 0 && g.scores.getOrDefault(SCENE, 0) == best) g.recommended.add(SCENE_AVATAR);   // hashes of an NPC's, a monster's or the glider's mesh
        g.evidence = why.toString();
    }
    /** Picker order: the recommended characters first, then everyone else in ID order. */
    public static List<Avatar> ordered(Guess g, String query) {
        List<Avatar> hits = search(query), out = new ArrayList<>();
        if (g != null) for (Avatar a : g.recommended) if (hits.contains(a)) out.add(a);
        for (Avatar a : hits) if (!out.contains(a)) out.add(a);
        return out;
    }
}
