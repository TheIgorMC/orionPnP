# OrionPnP Bill of Materials (BOM)

> This document outlines the complete bill of materials for building an OrionPnP machine.  
Each section describes the components required for different subsystems of the machine.  
PCB-specific components are listed separately in each board's repository folder.

---

## 📦 Mechanical

This section includes fasteners, aluminum extrusions, linear rails, and all structural hardware required for the main chassis and gantry assembly. It also has motion components, belts, steppers and such.

### Frame Structure

Price for mech. parts: € 38,56 


| Part ID                        | Part description                     | Qty.    | Price    | Total  | Source | Code               | Notes  |
| ------------------------------ | ------------------------------------ | ------- | -------- | ------ | ----- | ------------------- | ------ |
| 2020_478                       | 2020 profile – Length 478mm          | 1       | € 0,8560 | € 0,86 | JLCMC | TXCJ-H7-6-2020-L478 | X axis |
| 2020_450                       | 2020 profile – Length 450mm          | 4       | € 0,8062 | € 3,22 | JLCMC | TXCJ-H7-6-2020-L450 | Y body |
| 2020_435                       | 2020 profile – Length 435mm          | 6       | € 0,7792 | € 4,68 | JLCMC | TXCJ-H7-6-2020-L435 | X body |
| 2020_250                       | 2020 profile – Length 250mm          | 4       | € 0,4678 | € 1,87 | JLCMC | TXCJ-H7-6-2020-L250 | Vertical / z axis |
| Bracket_2020                   | 2020 profile angle bracket           | 30      | € 0,0671 | € 2,01 | JLCMC | TPDB-206-2020 |  |
| Tnut_M3                        | M3 drop-in T-nut                     | 300     | € 0,0090 | € 2,70 | JLCMC | TPAA-N-6-M3 | 100pcs+ |
| Tnut_M5                        | M5 drop-in T-nut                     | 300     | € 0,0055 | € 1,65 | JLCMC | TPAA-N-6-M5 | 100pcs+ |
| BallNut_M3                     | M3 spring-loaded ball nut            | 100     | € 0,0288 | € 2,88 | JLCMC | TPAB-N-6-M3 | 100pcs+ |
| BallNut_M4                     | M4 spring-loaded ball nut            | 50      | € 0,0294 | € 1,47 | JLCMC | TPAB-N-6-M4 | 50pcs+ |
| BallNut_M5                     | M5 spring-loaded ball nut            | 100     | € 0,0288 | € 2,88 | JLCMC | TPAB-N-6-M5 | 100pcs+ |
| HSCS_3x6                       | Hex socket cap screw – M3x6mm        | 100     | € 0,0037 | € 0,37 | JLCMC | EDLA-S1-M3-L6 | 100pcs+ |
| HSCS_3x10                      | Hex socket cap screw – M3x10mm       | 100     | € 0,0047 | € 0,47 | JLCMC | EDLA-S1-M3-L10 | 100pcs+ |
| HSCS_3x12                      | Hex socket cap screw – M3x12mm       | 100     | € 0,0051 | € 0,51 | JLCMC | EDLA-S1-M3-L12 | 100pcs+ |
| HSCS_3x18                      | Hex socket cap screw – M3x18mm       | 100     | € 0,0068 | € 0,68 | JLCMC | EDLA-S1-M3-L18 | 100pcs+ |
| HSCS_3x25                      | Hex socket cap screw – M3x25mm       | 100     | € 0,0085 | € 0,85 | JLCMC | EDLA-S1-M3-L25 | 100pcs+ |
| HSBH_3x6                       | Hex socket ball head screw – M3x6mm  | 100     | € 0,0031 | € 0,31 | JLCMC | EDLG-S1-M3-L6 | 100pcs+ |
| HSBH_3x10                      | Hex socket ball head screw – M3x10mm | 100     | € 0,0043 | € 0,43 | JLCMC | EDLG-S1-M3-L10 | 100pcs+ |
| HSBH_3x12                      | Hex socket ball head screw – M3x12mm | 100     | € 0,0042 | € 0,42 | JLCMC | EDLG-S1-M3-L12 | 100pcs+ |
| HSBH_3x18                      | Hex socket ball head screw – M3x18mm | 100     | € 0,0056 | € 0,56 | JLCMC | EDLG-S1-M3-L18 | 100pcs+ |
| HSBH_3x25                      | Hex socket ball head screw – M3x25mm | 100     | € 0,0073 | € 0,73 | JLCMC | EDLG-S1-M3-L25 | 100pcs+ |
| HSCS_4x10                      | Hex socket cap screw – M4x10mm       | 100     | € 0,0074 | € 0,74 | JLCMC | EDLA-S1-M4-L10 | 100pcs+ |
| HSCS_5x10                      | Hex socket cap screw – M5x10mm       | 100     | € 0,0105 | € 1,05 | JLCMC | EDLA-S1-M5-L10 | 100pcs+ |
| HSCS_5x20                      | Hex socket cap screw – M5x20mm       | 50      | € 0,0146 | € 0,73 | JLCMC | EDLA-S1-M5-L20 | 50pcs+ |
| Nut_Lock_M5                    | M5 locking nylock nut                | 50      | € 0,0116 | € 0,58 | JLCMC | EMLC-C2-Y-M5 | 50pcs+ |
| WASH_M3                        | M3 Washer – Standard                 | 100     | € 0,0011 | € 0,11 | JLCMC | EPDA-S1W-B-3 | 100pcs+ |
| WASH_M4_W                      | M4 washer – Wide                     | 50      | € 0,0030 | € 0,15 | JLCMC | EPDA-S1W-K-4 | 50pcs+ |
| WASH_M5                        | M5 Washer – Standard                 | 100     | € 0,0019 | € 0,19 | JLCMC | EPDA-S1W-B-5 | 100pcs+ |
| M3_Insert                      | M3 threaded insert – Standard        | 100     | € 0,0546 | € 5,46 | Ali | [Link](https://www.aliexpress.com/item/1005003582355741.html?spm=a2g0o.order_list.order_list_main.160.79341802HGwDOj) | M3 – OD4.5 – 4mm |
| M5_Insert                      | M5 threaded insert – Standard        | 20      | € 0,0680 | € 1,36 | Ali | [Link](https://www.aliexpress.com/item/1005003582355741.html?spm=a2g0o.order_list.order_list_main.160.79341802HGwDOj) | M5 – OD7.5 – 5mm |

---

## ⚙️ Motion

| Part ID                        | Part description                     | Qty.    | Price    | Total  | Source | Code               | Notes  |
| ------------------------------ | ------------------------------------ | ------- | -------- | ------ | ----- | ------------------- | ------ |
| Rail_400mm                                  | MGN12H Rail – 400mm                          | 3        | € 25,9800 | € 77,94 | Amazon | B08G1GVK7Q | 400mm |
| Rail_100mm                                  | MGN9H Rail – 100mm                           | 2        | € 17,7900 | € 35,58 | Amazon | B09FSG4KG3 | 100mm |
| Chain_Wide                                  | 18x37mm cable chain                          | 2        | € 9,2900 | € 18,58 | Ali | [Link](https://www.aliexpress.com/item/32852122851.html?spm=a2g0o.cart.0.0.38af38dawF4YnP&mp=1&pdp_npi=5@dis!EUR!EUR%2011.33!EUR%209.29!!EUR%208.59!!!@210390b817465198615193778e3863!65257125725!ct!IT!910753856!!2!0) | 18x37mm radius 38mm |
| Idler_6B5_GT2                               | Toothed GT2 idler for 6mm belt – 5mm bore    | 7        | € 2,6690 | € 18,68 | Ali | [Link](https://www.aliexpress.com/item/1005004957508089.html?spm=a2g0o.store_pc_allItems_or_groupList.0.0.6407458bkNkOXZ&pdp_npi=4@dis!EUR!€%203%2C43!€%203%2C43!!!3.78!3.78!@210390b817465199995396778e3863!12000031185339695!sh!IT!910753856!X) | Select 10pcs, 5mm bore |
| Idler_6B5_Smooth                            | Smooth idler for 6mm belt – 5mm bore         | 2        | € 2,6790 | € 5,36 | Ali | [Link](https://www.aliexpress.com/item/1005004957508089.html?spm=a2g0o.store_pc_allItems_or_groupList.0.0.6407458bkNkOXZ&pdp_npi=4@dis!EUR!€%203%2C43!€%203%2C43!!!3.78!3.78!@210390b817465199995396778e3863!12000031185339695!sh!IT!910753856!X) | Select 10pcs, 5mm bore |
| Pulley_6B5_20T                              | 20T pulley for GT2 belts – 5mm bore          | 4        | € 3,0650 | € 12,26 | Ali | [Link](https://www.aliexpress.com/item/32816906105.html?spm=a2g0o.store_pc_allItems_or_groupList.0.0.6407458bkNkOXZ&pdp_npi=4@dis!EUR!€%203%2C34!€%203%2C34!!!3.68!3.68!@210390b817465199995396778e3863!66526242417!sh!IT!910753856!X) | Select 6pcs, 5mm bore |
| GT2_Belt                                    | Gates GT2 belt – 6mm                         | 1        | € 18,5900 | € 18,59 | Ali | [Link](https://www.aliexpress.com/item/1005006507781085.html?spm=a2g0o.detail.pcDetailTopMoreOtherSeller.3.2ea85hQA5hQAO2&gps-id=pcDetailTopMoreOtherSeller&scm=1007.40050.354490.0&scm_id=1007.40050.354490.0&scm-url=1007.40050.354490.0&pvid=0f58cc73-9ff9-47ea-8e2f-682e2c40a00d&_t=gps-id:pcDetailTopMoreOtherSeller,scm-url:1007.40050.354490.0,pvid:0f58cc73-9ff9-47ea-8e2f-682e2c40a00d,tpp_buckets:668%232846%238107%231934&pdp_ext_f=%7B%22order%22%3A%22953%22%2C%22eval%22%3A%221%22%2C%22sceneId%22%3A%2230050%22%7D&pdp_npi=4%40dis%21EUR%218.19%218.19%21%21%219.03%219.03%21%402103894417465200153966833ea9ff%2112000037754698142%21rec%21IT%21910753856%21X&utparam-url=scene%3ApcDetailTopMoreOtherSeller%7Cquery_from%3A) | Select 4m |
| Stepper_17_2A                               | Nema17 stepper – 2A current – PCB plugs      | 3        | € 11,5300 | € 34,59 | Ali | [Link](https://www.aliexpress.com/item/1005004731197516.html?spm=a2g0o.detail.pcDetailTopMoreOtherSeller.2.32defW9jfW9jfD&gps-id=pcDetailTopMoreOtherSeller&scm=1007.40050.354490.0&scm_id=1007.40050.354490.0&scm-url=1007.40050.354490.0&pvid=6313c9a0-307d-4983-9518-3773d1680322&_t=gps-id:pcDetailTopMoreOtherSeller,scm-url:1007.40050.354490.0,pvid:6313c9a0-307d-4983-9518-3773d1680322,tpp_buckets:668%232846%238107%231934&pdp_ext_f=%7B%22order%22%3A%22338%22%2C%22eval%22%3A%221%22%2C%22sceneId%22%3A%2230050%22%7D&pdp_npi=4@dis!EUR!23.55!17.19!!!25.97!18.96!@210384cc17465239866344829ebe6b!12000030302442011!rec!IT!910753856!X&utparam-url=scene%3ApcDetailTopMoreOtherSeller%7Cquery_from%3A) | Select 3pcs |
| Stepper_17_1A                               | Nema17 stepper – 2A current – wire – pancake | 1        | € 11,3900 | € 11,39 | Ali | [Link](https://www.aliexpress.com/item/32585429251.html?pdp_npi=4@dis!EUR!€%2011%2C39!€%2011%2C39!!!12.56!12.56!@211b813f17465242330216289e1c93!65367836295!sh!IT!910753856!X&spm=a2g0o.store_pc_allItems_or_groupList.new_all_items_2007599132608.32585429251) |  |


---

## 🔌 Electronics

This section covers off-the-shelf electronic modules and wiring for the system.  
PCBs such as the mainboard and host/feeder boards have **their own BOMs in their project folders**.

| Part ID                        | Part description                     | Qty.    | Price    | Total  | Source | Code               | Notes  |
| ------------------------------ | ------------------------------------ | ------- | -------- | ------ | ----- | ------------------- | ------ |
| LRS-350-24                       | Meanwell LRS-350-24 Switching PSU – 350W – 24V | 1        | € 49,3900 | € 49,39 | Ali | [Link](https://www.aliexpress.com/item/32857859551.html?pdp_npi=4@dis!EUR!€%2053%2C68!€%2049%2C39!!!59.20!54.47!@211b618e17465226796515990ef6ec!65365302140!sh!IT!910753856!X&spm=a2g0o.store_pc_allItems_or_groupList.new_all_items_2007447399652.32857859551) | 350W – 24V |
| Orion_Core                       | Orion core v0.2a mainboard                     | 1        | € 175,0000 | € 175,00 | JLCPCB | n/a | Indicative pricing |
| Orion_Light                      | Orion camera light – 12V                       | 2        | € 12,0000 | € 24,00 | JLCPCB | n/a | Indicative pricing |
| IEC_Switch                       | IEC socket with integrated switch and fuse     | 1        | € 12,1300 | € 12,13 | LCSC | C17570428 |  |
| USB_Cam                          | ELP USB camera                                 | 2        | € 31,0900 | € 62,18 | Ali | [Link](https://www.aliexpress.com/item/32974277512.html?spm=a2g0o.order_list.order_list_main.563.79341802HGwDOj) | 1x 3,6mm – 1x 8mm |
---

## 🤖 Feeders

Base materials and non-electrical parts for building tape feeders.

| Part ID                        | Part description                     | Qty.    | Price    | Total  | Source | Code               | Notes  |
| ------------------------------ | ------------------------------------ | ------- | -------- | ------ | ----- | ------------------- | ------ |


For electronics and firmware, see the [Feeder Design](../../wiki/Feeder-Design) and [Command Reference](../../wiki/OrionProtocol) pages.

---

## 🔄 Pneumatics

Components related to vacuum generation and switching.

- 12V diaphragm vacuum pump
- Vacuum tubing (4mm or 6mm ID)
- Vacuum solenoids (12V, normally closed)
- Push-fit fittings
- Optional: vacuum reservoir
- Differential pressure sensors (I²C, one per nozzle)

---

## 🧩 3D Printed Parts

This section describes all printed components, organized by function.  
All models are included in the `/printable/` folder or will be provided as links in the [Build Your Own](../../wiki/Build-Your-Own) page.

### Head Assembly
- Nozzle carriage
- Camera holder (bottom view or upward-facing)
- Toolhead accessories

### X Axis
- Motor mount
- Belt clamp
- Endstop mount

### Y Axis
- Idler bracket
- Frame connector
- Rail mount

### Frame
- Feet or base frame adapters
- Corner reinforcements
- Panel mounts or covers

### Feeders
- Feeder body
- Tape guide
- Motor/gearbox holder
- Sensor mount
- Pogo interface frame

### Accessories
- Screen mount
- Encoder knob adapter
- Cable clips
- Feeder power switch lever (if used)

---

## Notes

- For recommended vendors, quantity breakdowns, and part numbers, see the full CSV or spreadsheet BOM.
- For any PCB-specific components, refer to the BOM in that board’s GitHub subfolder.

