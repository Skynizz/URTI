/*
 * KRC : côté CPU du rééclairage exact (KrcRelight.h). Indépendant du moteur, C++17, sans dépendance.
 *
 * À chaque frame, le moteur donne la puissance de chaque lampe (intensité × luminance de la couleur)
 * et son groupe (0..2 suivis, 3 fixe) ; KrcRelightHost calcule le rapport « après / avant » de chaque
 * groupe. Un groupe est inconnu (rapport 1, non appliqué) si ses lampes ne changent pas toutes du même
 * rapport ou si l'une s'allume depuis zéro : sa part dans les cellules ne dit alors rien de la
 * nouvelle lumière.
 *
 *   KrcRelightHost host;
 *   host.BeginFrame(cacheCleared);
 *   for (lampe i) host.AddLight(i, group, intensity * luminance(color));
 *   KrcRelightFrame f = host.EndFrame();
 *   // f.ratio, f.lightGroups -> KrcRelightParameters ; f.event -> passe KrcRelightEntry avant la
 *   // mise à jour de SHaRC ; f.active -> S différent de 1 quelque part (débruiteur)
 */

#ifndef KRC_RELIGHT_HOST_H
#define KRC_RELIGHT_HOST_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

struct KrcRelightFrame
{
    float ratio[3] = { 1.0f, 1.0f, 1.0f };
    uint32_t lightGroups = 0; // groupe de la lampe i aux bits 2i..2i+1
    bool event = false;       // un rapport connu est à appliquer cette frame
    bool active = false;      // un rééclairage a eu lieu depuis la remise à zéro du cache
};

class KrcRelightHost
{
public:
    void BeginFrame(bool cacheCleared)
    {
        m_frame = KrcRelightFrame();
        for (int g = 0; g < 3; g++)
        {
            m_seen[g] = false;
            m_known[g] = true;
        }
        if (cacheCleared)
            m_active = false;
    }

    // lightIndex : indice stable de la lampe d'une frame à l'autre (16 au plus pour lightGroups)
    void AddLight(uint32_t lightIndex, int group, float power)
    {
        group = std::min(std::max(group, 0), 3);
        if (lightIndex < 16)
            m_frame.lightGroups |= uint32_t(group) << (2 * lightIndex);
        if (m_prevPower.size() <= lightIndex)
            m_prevPower.resize(lightIndex + 1, -1.0f);

        const float prev = m_prevPower[lightIndex] < 0.0f ? power : m_prevPower[lightIndex];
        m_prevPower[lightIndex] = power;
        if (group == 3 || (prev <= 0.0f && power <= 0.0f))
            return;
        if (prev <= 0.0f)
        {
            m_known[group] = false; // s'allume depuis zéro : rapport inconnu
            return;
        }
        const float r = power / prev;
        if (!m_seen[group])
        {
            m_frame.ratio[group] = r;
            m_seen[group] = true;
        }
        else if (std::abs(r - m_frame.ratio[group]) > 1e-4f * std::max(r, m_frame.ratio[group]))
            m_known[group] = false;
    }

    KrcRelightFrame EndFrame()
    {
        for (int g = 0; g < 3; g++)
        {
            if (!m_known[g])
                m_frame.ratio[g] = 1.0f;
            m_frame.event = m_frame.event || m_frame.ratio[g] != 1.0f;
        }
        m_active = m_active || m_frame.event;
        m_frame.active = m_active;
        return m_frame;
    }

private:
    KrcRelightFrame m_frame;
    std::vector<float> m_prevPower;
    bool m_seen[3] = {};
    bool m_known[3] = {};
    bool m_active = false;
};

#endif // KRC_RELIGHT_HOST_H
